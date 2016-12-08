#include "core.h"
#include "log.h"
#include "core_model.h"
#include "micro_op_performance_model.h"
#include "branch_predictor.h"
#include "stats.h"
#include "core_model.h"
#include "dvfs_manager.h"
#include "subsecond_time.h"
#include "micro_op.h"
#include "allocator.h"
#include "config.hpp"

#include <cstdio>
#include <algorithm>

MicroOp* MicroOpPerformanceModel::m_serialize_uop = NULL;
MicroOp* MicroOpPerformanceModel::m_mfence_uop = NULL;
MicroOp* MicroOpPerformanceModel::m_memaccess_uop = NULL;

MicroOpPerformanceModel::MicroOpPerformanceModel(Core *core, bool issue_memops)
    : PerformanceModel(core)
    , m_core_model(CoreModel::getCoreModel(Sim()->getCfg()->getStringArray("perf_model/core/core_model", core->getId())))
    , m_allocator(m_core_model->createDMOAllocator())
    , m_issue_memops(issue_memops)
    , m_state_uops_done(false)
    , m_state_icache_done(false)
    , m_state_num_reads_done(0)
    , m_state_num_writes_done(0)
    , m_state_num_nonmem_done(0)
    , m_cache_lines_read(0)
    , m_cache_lines_written(0)
    , m_state_insn_period(ComponentPeriod::fromFreqHz(1)) // ComponentPeriod() is private, this is a placeholder.  Will be updated at resetState()
    , m_dyninsn_count(0)
    , m_dyninsn_cost(0)
    , m_dyninsn_zero_count(0)
{
   registerStatsMetric("performance_model", core->getId(), "dyninsn_count", &m_dyninsn_count);
   registerStatsMetric("performance_model", core->getId(), "dyninsn_cost", &m_dyninsn_cost);
   registerStatsMetric("performance_model", core->getId(), "dyninsn_zero_count", &m_dyninsn_zero_count);
#if DEBUG_DYN_INSN_LOG
   String filename;
   filename = "sim.dyninsn_log." + itostr(core->getId());
   filename = Sim()->getConfig()->formatOutputFileName(filename);
   m_dyninsn_log = std::fopen(filename.c_str(), "w");
#endif
#if DEBUG_INSN_LOG
   String insn_filename;
   insn_filename = "sim.insn_log." + itostr(core->getId());
   insn_filename = Sim()->getConfig()->formatOutputFileName(insn_filename);
   m_insn_log = std::fopen(insn_filename.c_str(), "w");
#endif
#if DEBUG_CYCLE_COUNT_LOG
   String cycle_filename;
   cycle_filename = "sim.cycle_log." + itostr(core->getId());
   cycle_filename = Sim()->getConfig()->formatOutputFileName(cycle_filename);
   m_cycle_log = std::fopen(cycle_filename.c_str(), "w");
#endif

   m_cpiITLBMiss = SubsecondTime::Zero();
   m_cpiDTLBMiss = SubsecondTime::Zero();
   registerStatsMetric("performance_model", core->getId(), "cpiITLBMiss", &m_cpiITLBMiss);
   registerStatsMetric("performance_model", core->getId(), "cpiDTLBMiss", &m_cpiDTLBMiss);

   m_cpiUnknown = SubsecondTime::Zero();
   registerStatsMetric("performance_model", core->getId(), "cpiUnknown", &m_cpiUnknown);

   m_cpiMemAccess = SubsecondTime::Zero();
   registerStatsMetric("performance_model", core->getId(), "cpiSyncMemAccess", &m_cpiMemAccess);

   if (! m_serialize_uop) {
      m_serialize_uop = new MicroOp();
      UInt64 interval_sync_cost = 1;
      m_serialize_uop->makeDynamic("DynamicInsn-Serialize", interval_sync_cost);
      m_serialize_uop->setSerializing(true);
      m_serialize_uop->setFirst(true);
      m_serialize_uop->setLast(true);
   }

   if (! m_mfence_uop) {
      m_mfence_uop = new MicroOp();
      m_mfence_uop->makeDynamic("DynamicInsn-MFENCE", 1);
      m_mfence_uop->setMemBarrier(true);
      m_mfence_uop->setFirst(true);
      m_mfence_uop->setLast(true);
   }

   if (! m_memaccess_uop) {
      m_memaccess_uop = new MicroOp();
      m_memaccess_uop->makeLoad(
           0 // uop offset of 0 (first uop)
         , XED_ICLASS_INVALID // opcode
         , "invalid"  // instructionName
         , 8
      );
      m_memaccess_uop->setMemBarrier(true);
      m_memaccess_uop->setFirst(true);
      m_memaccess_uop->setLast(true);
   }

   resetState(); // Needed to clear the period value
}

MicroOpPerformanceModel::~MicroOpPerformanceModel()
{
#if DEBUG_DYN_INSN_LOG
   std::fclose(m_dyninsn_log);
#endif
#if DEBUG_INSN_LOG
   std::fclose(m_insn_log);
#endif
#if DEBUG_CYCLE_COUNT_LOG
   std::fclose(m_cycle_log);
#endif
   delete m_allocator;
}

void MicroOpPerformanceModel::doSquashing(uint32_t first_squashed)
{
   MicroOp::uop_type_t uop_type = MicroOp::UOP_INVALID;
   uint32_t i, size, squashedCount = 0, microOpTypeOffset = 0;

   // recalculate microOpTypeOffsets for dynamic uops and
   // collect squashing info
   for(i = 0, size = m_current_uops.size(); i < size; i++)
   {
      DynamicMicroOp* uop = m_current_uops[i];
      if(uop->isSquashed())
      {
         squashedCount++;
      }
      else
      {
         if(uop->getMicroOp()->getType() != uop_type)
         {
            uop_type = uop->getMicroOp()->getType();
            microOpTypeOffset = 0;
         }
         uop->setMicroOpTypeOffset(microOpTypeOffset++);
      }
      uop->setSquashedCount(squashedCount);
   }

   // recalculate intraInstructionDependancies
   for(i = first_squashed; i < size; i++)
   {
      DynamicMicroOp* uop = m_current_uops[i];
      if(!uop->isSquashed())
      {
         uint32_t intraDeps = uop->getIntraInstrDependenciesLength();
         if(intraDeps != 0)
         {
            uint32_t iBase = i - uop->getMicroOp()->microOpTypeOffset;
            LOG_ASSERT_ERROR(iBase >= intraDeps, "intraInstructionDependancies (%d) should be <= (%d)", intraDeps, iBase);
            uop->setIntraInstrDependenciesLength(intraDeps - (m_current_uops[iBase]->getSquashedCount() -
                                           m_current_uops[iBase-intraDeps]->getSquashedCount()));
         }
      }
   }
}

bool MicroOpPerformanceModel::handleInstruction(Instruction const* instruction)
{

   printf("\n CAP: MicroOpPerformanceModel::handleInstruction: Instruction addr: 0x%x", instruction->getAddress());

   if (m_state_instruction == NULL)
   {
      m_state_instruction = instruction;
   }
   LOG_ASSERT_ERROR(m_state_instruction == instruction, "Error: The instruction has changed, but the internal state has not been reset!");

   // Get the period (current CPU frequency) for this instruction
   // Keep it constant during it's execution
   if (m_state_insn_period.getPeriod() == SubsecondTime::Zero())
   {
      m_state_insn_period = *(const_cast<ComponentPeriod*>(static_cast<const ComponentPeriod*>(m_elapsed_time)));
   }

   // Keep our current state of what has already been processed
   // Each getDynamicInstructionInfo() might cause an exception, but
   // we need to be sure to save what we have already computed

   if (!m_state_uops_done)
   {
      if (instruction->getMicroOps())
      {
         for(std::vector<const MicroOp*>::const_iterator it = instruction->getMicroOps()->begin(); it != instruction->getMicroOps()->end(); it++)
         {
            m_current_uops.push_back(m_core_model->createDynamicMicroOp(m_allocator, *it, m_state_insn_period));
         }
      }
      m_state_uops_done = true;
   }

   // Find some information
   size_t num_loads = 0;
   size_t num_stores = 0;
   size_t exec_base_index = SIZE_MAX;
   // Find the first load
   size_t load_base_index = SIZE_MAX;
   // Find the first store
   size_t store_base_index = SIZE_MAX;
   for (size_t m = 0 ; m < m_current_uops.size() ; m++ )
   {
      if (m_current_uops[m]->getMicroOp()->isExecute())
      {
         exec_base_index = m;
      }
      if (m_current_uops[m]->getMicroOp()->isStore())
      {
         ++num_stores;
         if (store_base_index == SIZE_MAX)
            store_base_index = m;
      }
      if (m_current_uops[m]->getMicroOp()->isLoad())
      {
         ++num_loads;
         if (load_base_index == SIZE_MAX)
            load_base_index = m;
      }
   }

   // Compute the iCache cost, and add to our cycle time
   if (!m_state_icache_done && Sim()->getConfig()->getEnableICacheModeling())
   {
      // Sometimes, these aren't real instructions (INST_SPAWN, etc), and therefore, we need to skip these
      if (instruction->getAddress() && !instruction->isDynamic() && m_current_uops.size() > 0 )
      {
         MemoryResult memres = getCore()->readInstructionMemory(instruction->getAddress(), instruction->getSize());

         // For the interval model, for now, use integers for the cycle latencies
         UInt64 memory_cycle_latency = SubsecondTime::divideRounded(memres.latency, m_state_insn_period);

         // Set the hit_where information for the icache
         // The interval model will only add icache latencies if there hasn't been a hit.
         m_current_uops[0]->setICacheHitWhere(memres.hit_where);
         m_current_uops[0]->setICacheLatency(memory_cycle_latency);
      }
      m_state_icache_done = true;
   }

   bool do_squashing = false;
   // Graphite instruction operands
   const OperandList &ops = instruction->getOperands();

   // If we haven't gotten all of our read or write data yet, iterate over the operands
   for (size_t i = 0 ; i < ops.size() ; ++i)
   {
      const Operand &o = ops[i];

      if (o.m_type == Operand::MEMORY)
      {  printf("\n CAP: Handle Instr Before Dyn Info");
         // For each memory operand, there exists a dynamic instruction to process
         DynamicInstructionInfo *info = getDynamicInstructionInfo(*instruction, m_issue_memops);
         printf("\n CAP: Handle Instr After Dyn Info %d", info);
         if (!info)
            return false;

         // Because the interval model is currently in cycles, convert the data to cycles here before using it
         // Force the latencies into cycles for use in the original interval model
         // FIXME Update the Interval Timer to use SubsecondTime
         UInt64 memory_cycle_latency = SubsecondTime::divideRounded(info->memory_info.latency, m_state_insn_period);
         /*if (instruction->getAddress() == 200)
					printf("\nS%" PRIu64 " - %" PRId64, 
					info->memory_info.latency.getInternalDataForced(), 
																						memory_cycle_latency);
         if (instruction->getAddress() == 100)
					printf("\nL%" PRIu64 " - %" PRId64, 
					info->memory_info.latency.getInternalDataForced(), 
																						memory_cycle_latency);*/

         // Optimize multiple accesses to the same cache line by one instruction (vscatter/vgather)
         //   For simplicity, vgather/vscatter have 16 load/store microops, one for each address.
         //   Here, we squash microops that touch a given cache line a second time
         //   FIXME: although the microop is squashed and its latency ignored, the cache still sees the access
         IntPtr cache_line = info->memory_info.addr & ~63; // FIXME: hard-coded cache line size

         if (o.m_direction == Operand::READ)
         {

            // Operand::READ

            if (load_base_index != SIZE_MAX)
            {

               size_t load_index = load_base_index + m_state_num_reads_done;

               LOG_ASSERT_ERROR(info->type == DynamicInstructionInfo::MEMORY_READ,
                                "Expected memory read info, got: %d.", info->type);
               LOG_ASSERT_ERROR(load_index < m_current_uops.size(),
                                "Expected load_index(%x) to be less than uops.size()(%d).", load_index, m_current_uops.size());
               LOG_ASSERT_ERROR(m_current_uops[load_index]->getMicroOp()->isLoad(),
                                "Expected uop %d to be a load.", load_index);

               if (std::find(m_cache_lines_read.begin(), m_cache_lines_read.end(), cache_line) != m_cache_lines_read.end())
               {
                  m_current_uops[load_index]->squash(&m_current_uops);
                  do_squashing = true;
               }
               m_cache_lines_read.push_back(cache_line);

               // Update this uop with load latencies
               UInt64 bypass_latency = m_core_model->getBypassLatency(m_current_uops[load_index]);
               m_current_uops[load_index]->setExecLatency(memory_cycle_latency + bypass_latency);
               Memory::Access addr;
               addr.set(info->memory_info.addr);
               m_current_uops[load_index]->setAddress(addr);
               m_current_uops[load_index]->setDCacheHitWhere(info->memory_info.hit_where);
               ++m_state_num_reads_done;
            }
            else
            {
               LOG_PRINT_ERROR("Read operand count mismatch");
            }

         }
         else
         {
            // Operand::WRITE

            if (store_base_index != SIZE_MAX)
            {

               size_t store_index = store_base_index + m_state_num_writes_done;

               LOG_ASSERT_ERROR(info->type == DynamicInstructionInfo::MEMORY_WRITE,
                                "Expected memory write info, got: %d.", info->type);
               LOG_ASSERT_ERROR(store_index < m_current_uops.size(),
                                "Expected store_index(%d) to be less than uops.size()(%d).", store_index, m_current_uops.size());
               LOG_ASSERT_ERROR(m_current_uops[store_index]->getMicroOp()->isStore(),
                                "Expected uop %d to be a store. [%d|%s]", store_index, m_current_uops[store_index]->getMicroOp()->getType(), m_current_uops[store_index]->getMicroOp()->toString().c_str());

               if (std::find(m_cache_lines_written.begin(), m_cache_lines_written.end(), cache_line) != m_cache_lines_written.end())
               {
                  m_current_uops[store_index]->squash(&m_current_uops);
                  do_squashing = true;
               }
               m_cache_lines_written.push_back(cache_line);

               // Update this uop with store latencies.
               UInt64 bypass_latency = m_core_model->getBypassLatency(m_current_uops[store_index]);
               m_current_uops[store_index]->setExecLatency(memory_cycle_latency + bypass_latency);
               Memory::Access addr;
               addr.set(info->memory_info.addr);
               m_current_uops[store_index]->setAddress(addr);
               m_current_uops[store_index]->setDCacheHitWhere(info->memory_info.hit_where);
               ++m_state_num_writes_done;
               printf("\n CAP: For Inst at 0x%x, MEM STORE microop to addr: 0x%x", instruction->getAddress(), info->memory_info.addr);


							 /*#ifdef PIC_USE_VPIC
         			 if (instruction->getAddress() == 200) {
   								ComponentTime new_latency(m_elapsed_time.getLatencyGenerator());
      						new_latency.addCycleLatency(memory_cycle_latency 
																																+ bypass_latency);
   								m_elapsed_time.addLatency(new_latency);
							 }
							 #endif*/
            }
            else
            {
               LOG_PRINT_ERROR("Write operand count mismatch");
            }

         }

         // When we have finally finished processing this dynamic instruction, remove it from the queue
         popDynamicInstructionInfo();
      }
      else
      {
         ++m_state_num_nonmem_done;
      }

   }

   if(do_squashing > 0)
      doSquashing();

   // Make sure there was an Operand/DynamicInstructionInfo for each MicroOp
   // This should detect mismatches between decoding as done by fillOperandListMemOps and InstructionDecoder
   assert(m_state_num_reads_done == num_loads);
   assert(m_state_num_writes_done == num_stores);

   // Instruction cost resolution
   // Because getCost may fail if there are missing DynInstrInfo's, do not call getCost() anywhere else but here
   // If it fails, keep state because we are waiting for processing that needs to occur,
   // but some costs (I-cache access) have already been resolved
   SubsecondTime insn_cost = instruction->getCost(getCore());
   if (insn_cost == PerformanceModel::DyninsninfoNotAvailable())
      return false;

#if DEBUG_INSN_LOG
   if (insn_cost > 17)
   {
      fprintf(m_insn_log, "[%llu] ", (long long unsigned int)m_cycle_count);
      if (load_base_index != SIZE_MAX) {
         fprintf(m_insn_log, "L");
      }
      if (store_base_index != SIZE_MAX) {
         fprintf(m_insn_log, "S");
      }
      if (exec_base_index != SIZE_MAX) {
         fprintf(m_insn_log, "X");
#ifdef ENABLE_MICROOP_STRINGS
         fprintf(m_insn_log, "-%s:%s", instruction->getDisassembly().c_str(), instruction->getTypeName().c_str());
         fflush(m_insn_log);
#endif
      }
      fprintf(m_insn_log, "approx cost = %llu\n", (long long unsigned int)insn_cost);
   }
#endif


   if (instruction->getType() == INST_BRANCH)
   {
      const BranchInstruction *branch_instruction = dynamic_cast<const BranchInstruction*>(instruction);
      LOG_ASSERT_ERROR(branch_instruction != NULL, "Expected a BranchInstruction, but did not get one.");

      // Set whether the branch was mispredicted or not
      LOG_ASSERT_ERROR(m_current_uops[exec_base_index]->getMicroOp()->isBranch(), "Expected to find a branch here.");
      m_current_uops[exec_base_index]->setBranchMispredicted(branch_instruction->getIsMispredict());
      m_current_uops[exec_base_index]->setBranchTaken(branch_instruction->getIsTaken());
      m_current_uops[exec_base_index]->setBranchTarget(branch_instruction->getTargetAddress());
      // Do not update the execution latency of a branch instruction
      // The interval model will calculate the branch latency
   }

   // Insert an instruction into the interval model to indicate that time has passed
   uint32_t new_num_insns = 0;
   ComponentTime new_latency(m_elapsed_time.getLatencyGenerator()); // Get a new, empty holder for latency
   bool latency_out_of_band = false; // If new_latency contains any values not returned from IntervalTimer/RobTimer, we'll need to call synchronize()
   if(m_current_uops.size() > 0)
   {
      uint64_t new_latency_cycles;

      boost::tie(new_num_insns, new_latency_cycles) = simulate(m_current_uops);
      new_latency.addCycleLatency(new_latency_cycles);

#if DEBUG_INSN_LOG > 1
      fprintf(m_insn_log, "[%llu] ", (long long unsigned int)m_cycle_count);
      if (load_base_index != SIZE_MAX) {
         fprintf(m_insn_log, "L");
      }
      if (store_base_index != SIZE_MAX) {
         fprintf(m_insn_log, "S");
      }
      if (exec_base_index != SIZE_MAX) {
         fprintf(m_insn_log, "X");
#ifdef ENABLE_MICROOP_STRINGS
         fprintf(m_insn_log, "-%s", m_current_uops[exec_base_index].getInstructionOpcodeName().c_str());
#endif
      }
      fprintf(m_insn_log, "approx cost = %llu\n", (long long unsigned int)insn_cost);
#endif
   }
   else if (insn_cost > SubsecondTime::Zero() && instruction->getType() != INST_MEM_ACCESS)
   {
      // Handle all non-zero, non MemAccess instructions here

      // Mark this operation as a serialization instruction
      // It's cost needs to be added to the overall latency, and for accuracy
      //  it will help to be sure that the model knows about additional time
      //  used for overlapping events

      // These tend to be sync instructions
      // Because of timing issues (large synchronization deltas), take these latencies into account immediately
      //  Nevertheless, do not add the instruction cost into the interval model because the latency
      //  has already been taken into account here.  The interval model will serialize, flushing the old window

      std::vector<DynamicMicroOp*> uops;
      uops.push_back(m_core_model->createDynamicMicroOp(m_allocator, m_serialize_uop, m_state_insn_period));

      uint64_t new_latency_cycles;
      boost::tie(new_num_insns, new_latency_cycles) = simulate(uops);
      new_latency.addCycleLatency(new_latency_cycles);

      // Add the instruction cost immediately to prevent synchronization issues
      if (insn_cost > SubsecondTime::Zero())
      {
         new_latency.addLatency(insn_cost);
         latency_out_of_band = true;
      }

      if (instruction->getType() == INST_TLB_MISS)
      {
         TLBMissInstruction const* tlb_miss_insn = dynamic_cast<TLBMissInstruction const*>(instruction);
         LOG_ASSERT_ERROR(tlb_miss_insn != NULL, "Expected a TLBMissInstruction, but did not get one.");
         if (tlb_miss_insn->isIfetch())
            m_cpiITLBMiss += insn_cost;
         else
            m_cpiDTLBMiss += insn_cost;
      }
      else if (instruction->getType() == INST_UNKNOWN)
      {
         m_cpiUnknown += insn_cost;
      }
      else if ((instruction->getType() == INST_SYNC) || (instruction->getType() == INST_RECV))
      {
         LOG_PRINT_ERROR("The performance model expects non-idle instructions, not INST_SYNC or INST_RECV");
      }
      else
      {
         LOG_PRINT_ERROR("Unexpectedly received something other than a TLBMiss Instruction");
      }


#if DEBUG_DYN_INSN_LOG
      fprintf(m_dyninsn_log, "[%llu] %s: cost = %llu\n", (long long unsigned int)m_elapsed_time.getElapsedTime().getInternalDataForced(), instruction->getTypeName().c_str(), (long long unsigned int)insn_cost.getInternalDataForced());
#endif
   }
   else if (insn_cost > SubsecondTime::Zero())
   {

      // Handle non-zero MemAccess DynamicInstructions here

      // MemAccess instructions are memory overheads, and should be handled the same way that Long Latency Loads are
      // Currently, the simulator requires that latencies that return from the memory hierarchy go into effect
      // immediately.  If one tries to place these latencies as normal instructions into the interval model,
      // Graphite will still see a huge delta with this cpu's time, and then generate another, equally large
      // MemAccess instruction.  Therefore, as a work around, add the latencies to the

      MemAccessInstruction const* mem_dyn_insn = dynamic_cast<MemAccessInstruction const*>(instruction);
      LOG_ASSERT_ERROR(mem_dyn_insn != NULL, "Expected a MemAccessInstruction, but did not get one.");

      // Update uop with the necessary information for the MemAccess DynamicInstruction
      std::vector<DynamicMicroOp*> uops;
      DynamicMicroOp* uop = m_core_model->createDynamicMicroOp(m_allocator, m_memaccess_uop, m_state_insn_period);

      // Long latency load setup
      SubsecondTime cost_add_latency_now(SubsecondTime::Zero());
      uint32_t cost_add_latency_interval = 0;
      uint32_t cutoff = m_core_model->getLongLatencyCutoff();
      bool force_lll = false;
      // if we are a long latency load (0 == disable)
      if ((cutoff > 0) && (insn_cost > cutoff * m_state_insn_period.getPeriod()))
      {
         // Long latency load
         cost_add_latency_now = insn_cost;
         cost_add_latency_interval = 1;
         // Force this instruction as a LLL, even though we will be taking into account the latencies right away
         // This will flush the old window and also check for 2nd order effects (nested LLLs)
         // FIXME For possible next steps, be sure to pass real LLL value in when we want to compare LLL event lengths for higher accuracy
         force_lll = true;
      }
      else
      {
         // Normal load
         cost_add_latency_now = SubsecondTime::Zero();
         cost_add_latency_interval = SubsecondTime::divideRounded(insn_cost, m_state_insn_period.getPeriod());
      }

      Memory::Access data_address;
      data_address.set(mem_dyn_insn->getDataAddress());
      uop->setExecLatency(cost_add_latency_interval);
      uop->setForceLongLatencyLoad(force_lll);

      // This load's value needs to be registered
      // Right now, serialization instructions are assumed to be executes, and the load latencies are skipped
      //uop.setSerializing( mem_dyn_insn->isFence() );
      // Add this micro-op to the vector for submission
      if ( mem_dyn_insn->isFence() )
      {
         // Add memory fencing support to better simulate actual conditions
         // CMPXCHG instructions are called in mutex handlers, and their performance is about the same as MFENCEs
         uops.push_back(m_core_model->createDynamicMicroOp(m_allocator, m_mfence_uop, m_state_insn_period));
         //uops.push_back(serialize_uop);
         uops.push_back(uop);
         uops.push_back(m_core_model->createDynamicMicroOp(m_allocator, m_mfence_uop, m_state_insn_period));
         // Additionally, we need to think about serialization, and it's effect
         // In the case of a system call, the system will be serialized
         // This would matter only for the case when we initially don't have a lock
         //  and then we go into the OS only to get the lock and return without much
         //  wait.  In that case, the serialization effects could hit performance
         //  more than the memory barrier ones.
      }
      else
      {
         uops.push_back(uop);
      }

      // TODO Before simulating, iterate over the uops to mark them as first/last

      // Send this into the interval simulator
      uint64_t new_latency_cycles;
      boost::tie(new_num_insns, new_latency_cycles) = simulate(uops);
      new_latency.addCycleLatency(new_latency_cycles);

      // Add a potential LLL cost that needs to be registered right away
      if (cost_add_latency_now > SubsecondTime::Zero())
      {
         new_latency.addLatency(cost_add_latency_now);
         latency_out_of_band = true;
      }

      m_dyninsn_count++;
      //m_dyninsn_cost+=insn_cost; // FIXME

      m_cpiMemAccess += cost_add_latency_now;

#if DEBUG_DYN_INSN_LOG
      fprintf(m_dyninsn_log, "[%llu](MA) %s: cost = %llu\n", (long long unsigned int) m_elapsed_time.getElapsedTime().getInternalDataForced(), instruction->getTypeName().c_str(), (long long unsigned int) insn_cost.getInternalDataForced());
#endif
   }
   else
   {
      m_dyninsn_zero_count++;
   }
   m_instruction_count += new_num_insns;
   m_elapsed_time.addLatency(new_latency);
   /*printf("%d, %s[%lu]: [%llu]: latency = %lu, (L%lu,S%lu)\n", 
			new_num_insns,
			instruction->getTypeName().c_str(), 
			instruction->getAddress(),
		  (long long unsigned int)m_elapsed_time.getElapsedTime().getInternalDataForced(), 
			new_latency.getElapsedTime().getInternalDataForced(), 
			num_loads, num_stores);*/

   if (latency_out_of_band)
      notifyElapsedTimeUpdate();

#if DEBUG_CYCLE_COUNT_LOG
   fprintf(m_cycle_log, "[%s] latency=%d\n", itostr(m_elapsed_time).c_str(), itostr(new_latency.getElapsedTime()).c_str());
#endif

   // At the end, update our state to process the next instruction
   resetState();

   return true;
}

void MicroOpPerformanceModel::resetState()
{
   m_state_uops_done = false;
   m_state_icache_done = false;
   m_state_num_reads_done = 0;
   m_state_num_writes_done = 0;
   m_state_num_nonmem_done = 0;
   m_state_insn_period *= 0;
   m_state_instruction = NULL;
   m_cache_lines_read.clear();
   m_cache_lines_written.clear();
   m_current_uops.clear();
}
