#include "cache_cntlr.h"
#include "log.h"
#include "memory_manager.h"
#include "core_manager.h"
#include "simulator.h"
#include "subsecond_time.h"
#include "config.hpp"
#include "fault_injection.h"
#include "hooks_manager.h"
#include "cache_atd.h"
#include "shmem_perf.h"

#include <cstring>

// Define to allow private L2 caches not to take the stack lock.
// Works in most cases, but seems to have some more bugs or race conditions, preventing it from being ready for prime time.
//#define PRIVATE_L2_OPTIMIZATION

Lock iolock;
#if 0
#  define LOCKED(...) { ScopedLock sl(iolock); fflush(stderr); __VA_ARGS__; fflush(stderr); }
#  define LOGID() fprintf(stderr, "[%s] %2u%c [ %2d(%2d)-L%u%c ] %-25s@%3u: ", \
                     itostr(getShmemPerfModel()->getElapsedTime(Sim()->getCoreManager()->amiUserThread() ? ShmemPerfModel::_USER_THREAD : ShmemPerfModel::_SIM_THREAD)).c_str(), Sim()->getCoreManager()->getCurrentCoreID(), \
                     Sim()->getCoreManager()->amiUserThread() ? '^' : '_', \
                     m_core_id_master, m_core_id, m_mem_component < MemComponent::L2_CACHE ? 1 : m_mem_component - MemComponent::L2_CACHE + 2, \
                     m_mem_component == MemComponent::L1_ICACHE ? 'I' : (m_mem_component == MemComponent::L1_DCACHE  ? 'D' : ' '),  \
                     __FUNCTION__, __LINE__ \
                  );
#  define MYLOG(...) LOCKED(LOGID(); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");)
#  define DUMPDATA(data_buf, data_length) { for(UInt32 i = 0; i < data_length; ++i) fprintf(stderr, "%02x ", data_buf[i]); }
#else
#  define MYLOG(...) {}
#endif

namespace ParametricDramDirectoryMSI
{

char CStateString(CacheState::cstate_t cstate) {
   switch(cstate)
   {
      case CacheState::INVALID:           return 'I';
      case CacheState::SHARED:            return 'S';
      case CacheState::SHARED_UPGRADING:  return 'u';
      case CacheState::MODIFIED:          return 'M';
      case CacheState::EXCLUSIVE:         return 'E';
      case CacheState::OWNED:             return 'O';
      case CacheState::INVALID_COLD:      return '_';
      case CacheState::INVALID_EVICT:     return 'e';
      case CacheState::INVALID_COHERENCY: return 'c';
      default:                            return '?';
   }
}

const char * ReasonString(Transition::reason_t reason) {
   switch(reason)
   {
      case Transition::CORE_RD:     return "read";
      case Transition::CORE_RDEX:   return "readex";
      case Transition::CORE_WR:     return "write";
      case Transition::UPGRADE:     return "upgrade";
      case Transition::EVICT:       return "evict";
      case Transition::BACK_INVAL:  return "backinval";
      case Transition::COHERENCY:   return "coherency";
      default:                      return "other";
   }
}

MshrEntry make_mshr(SubsecondTime t_issue, SubsecondTime t_complete) {
   MshrEntry e;
   e.t_issue = t_issue; e.t_complete = t_complete;
   return e;
}

#ifdef ENABLE_TRACK_SHARING_PREVCACHES
PrevCacheIndex CacheCntlrList::find(core_id_t core_id, MemComponent::component_t mem_component)
{
   for(PrevCacheIndex idx = 0; idx < size(); ++idx)
      if ((*this)[idx]->m_core_id == core_id && (*this)[idx]->m_mem_component == mem_component)
         return idx;
   LOG_PRINT_ERROR("");
}
#endif

void CacheMasterCntlr::createSetLocks(UInt32 cache_block_size, UInt32 num_sets, UInt32 core_offset, UInt32 num_cores)
{
   m_log_blocksize = floorLog2(cache_block_size);
   m_num_sets = num_sets;
   m_setlocks.resize(m_num_sets, SetLock(core_offset, num_cores));
}

SetLock*
CacheMasterCntlr::getSetLock(IntPtr addr)
{
   return &m_setlocks.at((addr >> m_log_blocksize) & (m_num_sets-1));
}

void
CacheMasterCntlr::createATDs(String name, String configName, core_id_t master_core_id, UInt32 shared_cores, UInt32 size,
   UInt32 associativity, UInt32 block_size, String replacement_policy, CacheBase::hash_t hash_function)
{
   // Instantiate an ATD for each sharing core
   for(UInt32 core_id = master_core_id; core_id < master_core_id + shared_cores; ++core_id)
   {
      m_atds.push_back(new ATD(name + ".atd", configName, core_id, size, associativity, block_size, replacement_policy, hash_function));
   }
}

void
CacheMasterCntlr::accessATDs(Core::mem_op_t mem_op_type, bool hit, IntPtr address, UInt32 core_num)
{
   if (m_atds.size())
      m_atds[core_num]->access(mem_op_type, hit, address);
}

CacheMasterCntlr::~CacheMasterCntlr()
{
   delete m_cache;
   for(std::vector<ATD*>::iterator it = m_atds.begin(); it != m_atds.end(); ++it)
   {
      delete *it;
   }
}

CacheCntlr::CacheCntlr(MemComponent::component_t mem_component,
      String name,
      core_id_t core_id,
      MemoryManager* memory_manager,
      AddressHomeLookup* tag_directory_home_lookup,
      Semaphore* user_thread_sem,
      Semaphore* network_thread_sem,
      UInt32 cache_block_size,
      ComponentLatency ss_program_time,
      CacheParameters & cache_params,
      ShmemPerfModel* shmem_perf_model,
      bool is_last_level_cache):
   m_mem_component(mem_component),
   m_memory_manager(memory_manager),
   m_next_cache_cntlr(NULL),
   m_last_level(NULL),
   m_tag_directory_home_lookup(tag_directory_home_lookup),
   m_numFSMmatches(0),
   m_swizzleSwitch(new Byte[SWIZZLE_SWITCH_X * SWIZZLE_SWITCH_Y]), // CAP: adding swizzle switch in ctrlr
   m_currStateMask(new Byte[SWIZZLE_SWITCH_Y]),
   m_reportingSteInfo(new Byte[NUM_SUBARRAYS*cache_block_size]),
   m_startSTEMask(new Byte[NUM_SUBARRAYS*cache_block_size]),
   m_perfect(cache_params.perfect),
   m_coherent(cache_params.coherent),
   m_prefetch_on_prefetch_hit(false),
   m_l1_mshr(cache_params.outstanding_misses > 0),
   m_core_id(core_id),
   m_cache_block_size(cache_block_size),
   m_ss_program_time(ss_program_time),
   m_cache_writethrough(cache_params.writethrough),
   m_writeback_time(cache_params.writeback_time),
   m_data_access_time(cache_params.data_access_time),
   m_tags_access_time(cache_params.tags_access_time),
   m_next_level_read_bandwidth(cache_params.next_level_read_bandwidth),
   m_shared_cores(cache_params.shared_cores),
   m_user_thread_sem(user_thread_sem),
   m_network_thread_sem(network_thread_sem),
   m_last_remote_hit_where(HitWhere::UNKNOWN),
   m_shmem_perf(new ShmemPerf()),
   m_shmem_perf_global(NULL),
   m_shmem_perf_model(shmem_perf_model),
   m_logASCIISetIndex(8)  // CAP:   
{
   m_core_id_master = m_core_id - m_core_id % m_shared_cores;
   Sim()->getStatsManager()->logTopology(name, core_id, m_core_id_master);

   LOG_ASSERT_ERROR(!Sim()->getCfg()->hasKey("perf_model/perfect_llc"),
                    "perf_model/perfect_llc is deprecated, use perf_model/lX_cache/perfect instead");

   if (isMasterCache())
   {
      /* Master cache */
      m_master = new CacheMasterCntlr(name, core_id, cache_params.outstanding_misses, cache_params.pic_outstanding);
      m_master->m_cache = new Cache(name,
            "perf_model/" + cache_params.configName,
            m_core_id,
            cache_params.num_sets,
            cache_params.associativity,
            m_cache_block_size,
            cache_params.replacement_policy,
            CacheBase::SHARED_CACHE,
            CacheBase::parseAddressHash(cache_params.hash_function),
            Sim()->getFaultinjectionManager()
               ? Sim()->getFaultinjectionManager()->getFaultInjector(m_core_id_master, mem_component)
               : NULL);
      m_master->m_prefetcher = Prefetcher::createPrefetcher(cache_params.prefetcher, cache_params.configName, m_core_id, m_shared_cores);

      if (Sim()->getCfg()->getBoolDefault("perf_model/" + cache_params.configName + "/atd/enabled", false))
      {
         m_master->createATDs(name,
               "perf_model/" + cache_params.configName,
               m_core_id,
               m_shared_cores,
               cache_params.num_sets,
               cache_params.associativity,
               m_cache_block_size,
               cache_params.replacement_policy,
               CacheBase::parseAddressHash(cache_params.hash_function));
      }

      Sim()->getHooksManager()->registerHook(HookType::HOOK_ROI_END, __walkUsageBits, (UInt64)this, HooksManager::ORDER_NOTIFY_PRE);
   }
   else
   {
      /* Shared, non-master cache, we're just a proxy */
      m_master = getMemoryManager()->getCacheCntlrAt(m_core_id_master, mem_component)->m_master;
   }

   if (m_master->m_prefetcher)
      m_prefetch_on_prefetch_hit = Sim()->getCfg()->getBoolArray("perf_model/" + cache_params.configName + "/prefetcher/prefetch_on_prefetch_hit", core_id);

   bzero(&stats, sizeof(stats));

   registerStatsMetric(name, core_id, "loads", &stats.loads);
   registerStatsMetric(name, core_id, "stores", &stats.stores);
   registerStatsMetric(name, core_id, "load-misses", &stats.load_misses);
   registerStatsMetric(name, core_id, "store-misses", &stats.store_misses);
   // Does not work for loads, since the interval core model doesn't issue the loads until after the first miss has completed
   registerStatsMetric(name, core_id, "load-overlapping-misses", &stats.load_overlapping_misses);
   registerStatsMetric(name, core_id, "store-overlapping-misses", &stats.store_overlapping_misses);
   registerStatsMetric(name, core_id, "loads-prefetch", &stats.loads_prefetch);
   registerStatsMetric(name, core_id, "stores-prefetch", &stats.stores_prefetch);
   registerStatsMetric(name, core_id, "hits-prefetch", &stats.hits_prefetch);
   registerStatsMetric(name, core_id, "evict-prefetch", &stats.evict_prefetch);
   registerStatsMetric(name, core_id, "invalidate-prefetch", &stats.invalidate_prefetch);
   registerStatsMetric(name, core_id, "hits-warmup", &stats.hits_warmup);
   registerStatsMetric(name, core_id, "evict-warmup", &stats.evict_warmup);
   registerStatsMetric(name, core_id, "invalidate-warmup", &stats.invalidate_warmup);
   registerStatsMetric(name, core_id, "total-latency", &stats.total_latency);
   registerStatsMetric(name, core_id, "snoop-latency", &stats.snoop_latency);
   registerStatsMetric(name, core_id, "qbs-query-latency", &stats.qbs_query_latency);
   registerStatsMetric(name, core_id, "mshr-latency", &stats.mshr_latency);
   registerStatsMetric(name, core_id, "prefetches", &stats.prefetches);
   for(CacheState::cstate_t state = CacheState::CSTATE_FIRST; state < CacheState::NUM_CSTATE_STATES; state = CacheState::cstate_t(int(state)+1)) {
      registerStatsMetric(name, core_id, String("loads-")+CStateString(state), &stats.loads_state[state]);
      registerStatsMetric(name, core_id, String("stores-")+CStateString(state), &stats.stores_state[state]);
      registerStatsMetric(name, core_id, String("load-misses-")+CStateString(state), &stats.load_misses_state[state]);
      registerStatsMetric(name, core_id, String("store-misses-")+CStateString(state), &stats.store_misses_state[state]);
      registerStatsMetric(name, core_id, String("evict-")+CStateString(state), &stats.evict[state]);
      registerStatsMetric(name, core_id, String("backinval-")+CStateString(state), &stats.backinval[state]);
   }
   if (mem_component == MemComponent::L1_ICACHE || mem_component == MemComponent::L1_DCACHE) {
      for(HitWhere::where_t hit_where = HitWhere::WHERE_FIRST; hit_where < HitWhere::NUM_HITWHERES; hit_where = HitWhere::where_t(int(hit_where)+1)) {
         const char * where_str = HitWhereString(hit_where);
         if (where_str[0] == '?') continue;
         registerStatsMetric(name, core_id, String("loads-where-")+where_str, &stats.loads_where[hit_where]);
         registerStatsMetric(name, core_id, String("stores-where-")+where_str, &stats.stores_where[hit_where]);
      }
   }
   registerStatsMetric(name, core_id, "coherency-downgrades", &stats.coherency_downgrades);
   registerStatsMetric(name, core_id, "coherency-upgrades", &stats.coherency_upgrades);
   registerStatsMetric(name, core_id, "coherency-writebacks", &stats.coherency_writebacks);
   registerStatsMetric(name, core_id, "coherency-invalidates", &stats.coherency_invalidates);
#ifdef ENABLE_TRANSITIONS
   for(CacheState::cstate_t old_state = CacheState::CSTATE_FIRST; old_state < CacheState::NUM_CSTATE_STATES; old_state = CacheState::cstate_t(int(old_state)+1))
      for(CacheState::cstate_t new_state = CacheState::CSTATE_FIRST; new_state < CacheState::NUM_CSTATE_STATES; new_state = CacheState::cstate_t(int(new_state)+1))
         registerStatsMetric(name, core_id, String("transitions-")+CStateString(old_state)+"-"+CStateString(new_state), &stats.transitions[old_state][new_state]);
   for(Transition::reason_t reason = Transition::REASON_FIRST; reason < Transition::NUM_REASONS; reason = Transition::reason_t(int(reason)+1))
      for(CacheState::cstate_t old_state = CacheState::CSTATE_FIRST; old_state < CacheState::NUM_CSTATE_SPECIAL_STATES; old_state = CacheState::cstate_t(int(old_state)+1))
         for(CacheState::cstate_t new_state = CacheState::CSTATE_FIRST; new_state < CacheState::NUM_CSTATE_SPECIAL_STATES; new_state = CacheState::cstate_t(int(new_state)+1))
            registerStatsMetric(name, core_id, String("transitions-")+ReasonString(reason)+"-"+CStateString(old_state)+"-"+CStateString(new_state), &stats.transition_reasons[reason][old_state][new_state]);
#endif
   if (is_last_level_cache)
   {
      m_shmem_perf_global = new ShmemPerf();
      m_shmem_perf_totaltime = SubsecondTime::Zero();
      m_shmem_perf_numrequests = 0;

      for(int i = 0; i < ShmemPerf::NUM_SHMEM_TIMES; ++i)
      {
         ShmemPerf::shmem_times_type_t reason = ShmemPerf::shmem_times_type_t(i);
         registerStatsMetric(name, core_id, String("uncore-time-")+ShmemReasonString(reason), &m_shmem_perf_global->getComponent(reason));
      }
      registerStatsMetric(name, core_id, "uncore-totaltime", &m_shmem_perf_totaltime);
      registerStatsMetric(name, core_id, "uncore-requests", &m_shmem_perf_numrequests);
   }

		//#ifdef PIC_ENABLE_OPERATIONS
		m_pic_on = Sim()->getCfg()->getBool("general/pic_on");
		pic_other_load_address = 0;
		pic_other_load2_address = 0;
		pic_last_key_address = 0;
		if(m_pic_on) {
			if(Sim()->getCfg()->getBool("general/microbench_run")) {
				m_microbench_loopsize	= Sim()->getCfg()->getInt("general/microbench_loopsize");
				m_distinct_search_keys = 
					Sim()->getCfg()->getInt("general/microbench_totalsize")/
					m_microbench_loopsize;
				m_microbench_outer_loops = 
											Sim()->getCfg()->getInt("general/microbench_outer_loops");
				assert(m_microbench_outer_loops);
			}
			else
				m_microbench_loopsize = 0;
			m_pic_use_vpic = Sim()->getCfg()->getBool("general/pic_use_vpic");
			m_pic_avoid_dram = Sim()->getCfg()->getBool("general/pic_avoid_dram");
			m_pic_cache_level = Sim()->getCfg()->getInt("general/pic_cache_level");
    	for(HitWhere::where_t hit_where1 = HitWhere::WHERE_FIRST; 
								hit_where1 < HitWhere::NUM_HITWHERES; 
														hit_where1 = HitWhere::where_t(int(hit_where1)+1)) {
    		for(HitWhere::where_t hit_where2 = HitWhere::WHERE_FIRST; 
								hit_where2 < HitWhere::NUM_HITWHERES; 
														hit_where2 = HitWhere::where_t(int(hit_where2)+1)) {
    			const char * where_str1 = HitWhereString(hit_where1);
    			const char * where_str2 = HitWhereString(hit_where2);
    	    if (where_str1[0] == '?') continue;
    	    if (where_str2[0] == '?') continue;
    			//registerStatsMetric(name, core_id, 
						//String("pic-from-")+where_str1+ String("-to-")+where_str2, 
						//&stats.pics_where[(int)hit_where1][(int)hit_where2]);
				}
			}
    	for(CacheCntlr::pic_ops_t start = CacheCntlr::PIC_COPY; 
								start < CacheCntlr::NUM_PIC_OPS; 
								start = CacheCntlr::pic_ops_t(int(start)+1)) {
   			const char * op_str = picOpString(start);

    		registerStatsMetric(name, core_id, 
						String("pic_vops_")+op_str, &stats.pic_vops[(int)start]);

    		registerStatsMetric(name, core_id, 
						String("pic_ops_")+op_str, &stats.pic_ops[(int)start]);
    		registerStatsMetric(name, core_id, 
						String("pic_ops_tag_access_")+op_str, &stats.pic_ops_tag_access[(int)start]);
    		registerStatsMetric(name, core_id, 
						String("pic_ops_read_miss_")+op_str, &stats.pic_ops_read_miss[(int)start]);
    		registerStatsMetric(name, core_id, 
						String("pic_ops_write_miss_")+op_str, &stats.pic_ops_write_miss[(int)start]);

    		for(CacheCntlr::pic_map_policy_t map_start = 
																	CacheCntlr::PIC_ALL_WAYS_ONE_BANK; 
								map_start < CacheCntlr::NUM_PIC_MAP_POLICY; 
								map_start = CacheCntlr::pic_map_policy_t(int(map_start)+1)) {
   					const char * map_str = picMapString(map_start);
    				registerStatsMetric(name, core_id, 
						String("pic_ops_in_bank") + String("_") + op_str 
																			+ String("_") + map_str, 
										&stats.pic_ops_in_bank[(int)start][(int)map_start]);
				 }
    		registerStatsMetric(name, core_id, 
						String("pic_ops_inv")+op_str, &stats.pic_ops_inv[(int)start]);
    		registerStatsMetric(name, core_id, 
						String("pic_ops_wb")+op_str, &stats.pic_ops_wb[(int)start]);
			}
   		registerStatsMetric(name, core_id, "pic_key_writes", &stats.pic_key_writes);
   		registerStatsMetric(name, core_id, "pic_key_misses", &stats.pic_key_misses);
		}
		else {
			m_pic_use_vpic = false; 
			m_pic_avoid_dram = false;
			m_pic_cache_level = -1;
		}
   	registerStatsMetric(name, core_id, "dirty_evicts", &stats.dirty_evicts);
   	registerStatsMetric(name, core_id, "dirty_backinval", &stats.dirty_backinval);
   	registerStatsMetric(name, core_id, "writebacks", &stats.writebacks);

}

CacheCntlr::~CacheCntlr()
{
   if (isMasterCache())
   {
      delete m_master;
   }
   delete m_shmem_perf;
   if (m_shmem_perf_global)
      delete m_shmem_perf_global;
   #ifdef TRACK_LATENCY_BY_HITWHERE
   for(std::unordered_map<HitWhere::where_t, StatHist>::iterator it = lat_by_where.begin(); it != lat_by_where.end(); ++it) {
      printf("%2u-%s: ", m_core_id, HitWhereString(it->first));
      it->second.print();
   }
   #endif
}

void
CacheCntlr::setPrevCacheCntlrs(CacheCntlrList& prev_cache_cntlrs)
{
   /* Append our prev_caches list to the master one (only master nodes) */
   for(CacheCntlrList::iterator it = prev_cache_cntlrs.begin(); it != prev_cache_cntlrs.end(); it++)
      if ((*it)->isMasterCache())
         m_master->m_prev_cache_cntlrs.push_back(*it);
   #ifdef ENABLE_TRACK_SHARING_PREVCACHES
   LOG_ASSERT_ERROR(m_master->m_prev_cache_cntlrs.size() <= MAX_NUM_PREVCACHES, "shared locations vector too small, increase MAX_NUM_PREVCACHES to at least %u", m_master->m_prev_cache_cntlrs.size());
   #endif
}

void
CacheCntlr::setDRAMDirectAccess(DramCntlrInterface* dram_cntlr, UInt64 num_outstanding)
{
   m_master->m_dram_cntlr = dram_cntlr;
   m_master->m_dram_outstanding_writebacks = new ContentionModel("llc-evict-queue", m_core_id, num_outstanding);
}


/*****************************************************************************
 * operations called by core on first-level cache
 *****************************************************************************/
const char * picOpString(CacheCntlr::pic_ops_t pic_opcode) {
   switch(pic_opcode)
   {
      case CacheCntlr::PIC_COPY:		return "copy";
      case CacheCntlr::PIC_CMP:     return "cmp";
      case CacheCntlr::PIC_SEARCH:  return "search";
      case CacheCntlr::PIC_LOGICAL:  return "logic";
      case CacheCntlr::PIC_CLMULT:  return "clmult";
      default:                      return "????";
   }
}
const char * picMapString(CacheCntlr::pic_map_policy_t pic_policy) {
   switch(pic_policy)
   {
      case CacheCntlr::PIC_ALL_WAYS_ONE_BANK:			return "all_ways";
      case CacheCntlr::PIC_MORE_SETS_ONE_BANK:    return "more_sets";
      default:                      							return "????";
   }
}
void CacheCntlr::picUpdateCounters(CacheCntlr::pic_ops_t pic_opcode, 
  IntPtr ca_address1, HitWhere::where_t hit_where1, 
  IntPtr ca_address2, HitWhere::where_t hit_where2,
  IntPtr ca_address3, HitWhere::where_t hit_where3
	) {
	{
  	ScopedLock sl(getLock());
    stats.pic_ops[(int)pic_opcode]++;
    //stats.pics_where[(int)hit_where1][(int) hit_where2]++;
		if(m_microbench_loopsize && (pic_opcode == CacheCntlr::PIC_SEARCH)) {
			if(m_master->m_prev_cache_cntlrs.empty()) { //L1
				stats.pic_key_writes = 
					((m_microbench_loopsize < 2048) ? m_distinct_search_keys : 
						m_distinct_search_keys * (m_microbench_loopsize/2048)); 
			}
			else {	//L2
				stats.pic_key_writes = m_distinct_search_keys; 
			}
			stats.pic_key_writes = stats.pic_key_writes * (m_microbench_outer_loops);
			//is this the last key I saw?
			if(pic_last_key_address != ca_address2) {		//new key, check for miss
				if(hit_where2 != (HitWhere::where_t)m_mem_component) {//key_miss
					stats.pic_key_misses++;
					assert( stats.pic_key_misses <= stats.pic_key_writes);
				}
				pic_last_key_address = ca_address1; //remember key
			}
			stats.pic_key_writes -= stats.pic_key_misses;
		}

		if(hit_where1 != (HitWhere::where_t)m_mem_component)
			stats.pic_ops_read_miss[(int)pic_opcode]++;
		if((hit_where2 != (HitWhere::where_t)m_mem_component)) {
			if(pic_opcode == CacheCntlr::PIC_COPY) 
				stats.pic_ops_write_miss[(int)pic_opcode]++;
			else
				stats.pic_ops_read_miss[(int)pic_opcode]++;
		}
		
		if((hit_where3 != HitWhere::UNKNOWN) 
			&& (hit_where3 != (HitWhere::where_t)m_mem_component))
				stats.pic_ops_write_miss[(int)pic_opcode]++;

		if(!ca_address3) {
			UInt32 set1, way1;
			UInt32 set2, way2;
			m_master->m_cache->peekSingleLine(ca_address1, &set1, &way1);
			m_master->m_cache->peekSingleLine(ca_address2, &set2, &way2);

    	for(CacheCntlr::pic_map_policy_t map_start = 
				CacheCntlr::PIC_ALL_WAYS_ONE_BANK; 
				map_start < CacheCntlr::NUM_PIC_MAP_POLICY; 
				map_start = CacheCntlr::pic_map_policy_t(int(map_start)+1)) {
				if(inSameBank(m_mem_component, set1, way1, set2, way2, map_start))
    			stats.pic_ops_in_bank[(int)pic_opcode][(int)map_start]++;
			}
		}
	}
}

void CacheCntlr::picUpdateCorrections(CacheCntlr::pic_ops_t pic_opcode, 
					unsigned short inv, unsigned short wb) {
	{
  	ScopedLock sl(getLock());
    stats.pic_ops_inv[(int)pic_opcode] 	+= inv; 
    stats.pic_ops_wb[(int)pic_opcode]		+= wb;
    stats.pic_ops_tag_access[(int)pic_opcode]		+= 1;
	}
}
//Should this level be picked for this PIC OP?
bool CacheCntlr::pickCurLevelPicOp(CacheCntlr::pic_ops_t pic_opcode, 
IntPtr ca_address1, IntPtr ca_address2) {

	//Do tag check..if match do operation here
	if ((m_pic_cache_level == 0) && 
				m_master->m_prev_cache_cntlrs.empty()) //all L1
		return true;
	
	if ((m_pic_cache_level == 1) && 
					!(m_master->m_prev_cache_cntlrs.empty()))
		return true;
      
	if (!(m_master->m_prev_cache_cntlrs.empty())) { //aL2
  	if(getHome(ca_address1)	== getHome(ca_address2)) {	//pick L3
			return false;
		}
		else if(pic_opcode == PIC_SEARCH) { //Pretend same slice by picking a dummy address
			return false;
		}
		return true;
	}
	return false;
}
HitWhere::where_t
CacheCntlr::processPicVOpFromCoreLOGICAL(
			CacheCntlr::pic_ops_t pic_opcode,
      IntPtr ca_address1, IntPtr ca_address2, IntPtr ca_address3, UInt32 count)
{

	HitWhere::where_t hit_where = HitWhere::UNKNOWN;
	HitWhere::where_t this_hit_where;
	SubsecondTime t_pic_begin, t_pic_entry_avail, t_pic_end;
	{
  	ScopedLock sl(getLock());
    stats.pic_vops[(int)pic_opcode]++;
	}
	while(count) {
		//Model finite PIC Controller table
		{
  	  ScopedLock sl(getLock());
  	  t_pic_begin = getShmemPerfModel()->getElapsedTime(
																					ShmemPerfModel::_USER_THREAD);
  	  t_pic_entry_avail = t_pic_begin;
  	  t_pic_entry_avail = m_master->m_l1_pic_entries.getStartTime(t_pic_begin);
  	  SubsecondTime pic_entry_latency = t_pic_entry_avail - t_pic_begin;
  	  // Delay until we have an empty slot in the MSHR
  	  getShmemPerfModel()->incrElapsedTime(pic_entry_latency, 
			 												ShmemPerfModel::_USER_THREAD);
		}

  	this_hit_where = processPicSOpFromCoreLOGICAL(pic_opcode, ca_address1,
				ca_address2, ca_address3);
    if (hit_where == HitWhere::UNKNOWN || (this_hit_where > hit_where))
    	hit_where = this_hit_where;
		--count;

		ca_address1 += 64;
		ca_address2 += 64;
		ca_address3 += 64;
		{
    	t_pic_end = getShmemPerfModel()->getElapsedTime(
																			ShmemPerfModel::_USER_THREAD);
      ScopedLock sl(getLock());
      m_master->m_l1_pic_entries.getCompletionTime(t_pic_begin, 
														t_pic_end - t_pic_entry_avail);
    }
		LOG_PRINT("\nV%d+%lx..+%lx:  %lu ns- %lu ns - %lu ns", (int)pic_opcode, 
				ca_address1, ca_address2, t_pic_begin.getNS(), 
				t_pic_entry_avail.getNS(),
				t_pic_end.getNS());
	}
  return hit_where;
}
HitWhere::where_t
CacheCntlr::processPicVOpFromCoreCLMULT(
			CacheCntlr::pic_ops_t pic_opcode,
      IntPtr ca_address1, IntPtr ca_address2, 
			IntPtr ca_address3, UInt32 word_size)
{

	HitWhere::where_t hit_where = HitWhere::UNKNOWN;
	HitWhere::where_t this_hit_where;
	SubsecondTime t_pic_begin, t_pic_entry_avail, t_pic_end;
	{
  	ScopedLock sl(getLock());
    stats.pic_vops[(int)pic_opcode]++;
	}
	//Number of cache blocks we need
	//word_size is in bites. 64/128/256/512=#rows
	UInt32 rows_per_cb = (512/word_size);
	UInt32 count = (word_size/rows_per_cb);
	while(count) {
		//Model finite PIC Controller table
		{
  	  ScopedLock sl(getLock());
  	  t_pic_begin = getShmemPerfModel()->getElapsedTime(
																					ShmemPerfModel::_USER_THREAD);
  	  t_pic_entry_avail = t_pic_begin;
  	  t_pic_entry_avail = m_master->m_l1_pic_entries.getStartTime(t_pic_begin);
  	  SubsecondTime pic_entry_latency = t_pic_entry_avail - t_pic_begin;
  	  // Delay until we have an empty slot in the MSHR
  	  getShmemPerfModel()->incrElapsedTime(pic_entry_latency, 
			 												ShmemPerfModel::_USER_THREAD);
		}

  	this_hit_where = processPicSOpFromCore(pic_opcode, ca_address1,
				ca_address2);
    if (hit_where == HitWhere::UNKNOWN || (this_hit_where > hit_where))
    	hit_where = this_hit_where;
		--count;

		ca_address1 += 64;
		{
    	t_pic_end = getShmemPerfModel()->getElapsedTime(
																			ShmemPerfModel::_USER_THREAD);
      ScopedLock sl(getLock());
      m_master->m_l1_pic_entries.getCompletionTime(t_pic_begin, 
														t_pic_end - t_pic_entry_avail);
    }
		LOG_PRINT("\nVMULT%d+%lx..+%lx..+%lx:  %lu ns- %lu ns - %lu ns", (int)pic_opcode, 
				ca_address1, ca_address2, ca_address3, t_pic_begin.getNS(), 
				t_pic_entry_avail.getNS(),
				t_pic_end.getNS());
	}
	//Result accumulated: tags is 1 cycle
  getMemoryManager()->incrElapsedTime(m_mem_component, 
	CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);

  hit_where = processMemOpFromCore(
  	Core::NONE, Core::WRITE,
    ca_address3, 0, NULL, 64, true, true);
  //getMemoryManager()->incrElapsedTime(m_mem_component, 
	//CachePerfModel::ACCESS_CACHE_DATA, ShmemPerfModel::_USER_THREAD);
  return hit_where;
}

HitWhere::where_t
CacheCntlr::processPicVOpFromCore(
			CacheCntlr::pic_ops_t pic_opcode,
      IntPtr ca_address1, IntPtr ca_address2, UInt32 count)
{

	HitWhere::where_t hit_where = HitWhere::UNKNOWN;
	HitWhere::where_t this_hit_where;
	SubsecondTime t_pic_begin, t_pic_entry_avail, t_pic_end;
	{
  	ScopedLock sl(getLock());
    stats.pic_vops[(int)pic_opcode]++;
	}
	while(count) {
		//Model finite PIC Controller table
		{
  	  ScopedLock sl(getLock());
  	  t_pic_begin = getShmemPerfModel()->getElapsedTime(
																					ShmemPerfModel::_USER_THREAD);
  	  t_pic_entry_avail = t_pic_begin;
  	  t_pic_entry_avail = m_master->m_l1_pic_entries.getStartTime(t_pic_begin);
  	  SubsecondTime pic_entry_latency = t_pic_entry_avail - t_pic_begin;
  	  // Delay until we have an empty slot in the MSHR
  	  getShmemPerfModel()->incrElapsedTime(pic_entry_latency, 
			 												ShmemPerfModel::_USER_THREAD);
		}

  	this_hit_where = processPicSOpFromCore(pic_opcode, ca_address1,
				ca_address2);
    if (hit_where == HitWhere::UNKNOWN || (this_hit_where > hit_where))
    	hit_where = this_hit_where;
		--count;

		ca_address1 += 64;
		if(pic_opcode != PIC_SEARCH)
			ca_address2 += 64;		//Key is a constant address

		{
    	t_pic_end = getShmemPerfModel()->getElapsedTime(
																			ShmemPerfModel::_USER_THREAD);
      ScopedLock sl(getLock());
      m_master->m_l1_pic_entries.getCompletionTime(t_pic_begin, 
														t_pic_end - t_pic_entry_avail);
    }
		LOG_PRINT("\nV%d+%lx..+%lx:  %lu ns- %lu ns - %lu ns", (int)pic_opcode, 
				ca_address1, ca_address2, t_pic_begin.getNS(), 
				t_pic_entry_avail.getNS(),
				t_pic_end.getNS());
	}
  return hit_where;
}
HitWhere::where_t
CacheCntlr::processPicSOpFromCoreLOGICAL(
			CacheCntlr::pic_ops_t pic_opcode,
      IntPtr ca_address1, IntPtr ca_address2, IntPtr ca_address3)
{
	LOG_PRINT("\n%d+%lx..+%lx..+%lx", (int)pic_opcode, ca_address1, ca_address2, ca_address3);
  SubsecondTime t_start = getShmemPerfModel()->getElapsedTime(
																		ShmemPerfModel::_USER_THREAD);
	//First check if we want to do this operation here
	bool do_operation = 
										pickCurLevelPicOp(pic_opcode, ca_address1, ca_address2);
	if(do_operation) {

		pic_other_load_address 	= ca_address2;	//no-evict
		pic_other_load2_address = ca_address3;	//no-evict
  	HitWhere::where_t	hit_where1 = picProcessMemOpFromCore(
      Core::NONE, Core::READ,
      ca_address1, 0, NULL, 64, true, true);
  	SubsecondTime t_load1_end = getShmemPerfModel()->getElapsedTime
																				(ShmemPerfModel::_USER_THREAD);
  	//Start the store at the end of load processing not its *completion*
 		getShmemPerfModel()->setElapsedTime(ShmemPerfModel::_USER_THREAD, 
									t_start) ;
  	getMemoryManager()->incrElapsedTime(m_mem_component, 
			CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);
		
		pic_other_load_address = ca_address1;	//no-evict
		HitWhere::where_t hit_where2 = HitWhere::UNKNOWN;
  	hit_where2 = picProcessMemOpFromCore(
    Core::NONE, Core::READ,
    ca_address2, 0, NULL, 64, true, true);
  	SubsecondTime t_load2_end = getShmemPerfModel()->getElapsedTime
																				(ShmemPerfModel::_USER_THREAD);
 		getShmemPerfModel()->setElapsedTime(ShmemPerfModel::_USER_THREAD, 
									t_start) ;
  	getMemoryManager()->incrElapsedTime(m_mem_component, 
			CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);

		pic_other_load2_address 	= ca_address2;	//no-evict
		HitWhere::where_t hit_where3 = HitWhere::UNKNOWN;
  	hit_where3 = picProcessMemOpFromCore(
      Core::NONE, Core::WRITE,
      ca_address3, 0, NULL, 64, true, true);
  	SubsecondTime t_store_end = getShmemPerfModel()->getElapsedTime
																				(ShmemPerfModel::_USER_THREAD);
		pic_other_load_address 		= 0;
		pic_other_load2_address 	= 0;
		if(t_load1_end > t_store_end)
 			getShmemPerfModel()->setElapsedTime(ShmemPerfModel::_USER_THREAD, 
									t_load1_end);
		if(t_load2_end > t_load1_end)
 			getShmemPerfModel()->setElapsedTime(ShmemPerfModel::_USER_THREAD, 
									t_load2_end);

	 	//If any one was a miss, account for more TAG access
	 	if ((hit_where1 != (HitWhere::where_t)m_mem_component) || 
	  	(hit_where2 != (HitWhere::where_t)m_mem_component) || 
	  	(hit_where3 != (HitWhere::where_t)m_mem_component)
			) {
      	getMemoryManager()->incrElapsedTime(m_mem_component, 
					CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);
      	getMemoryManager()->incrElapsedTime(m_mem_component, 
				CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);
      	getMemoryManager()->incrElapsedTime(m_mem_component, 
				CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);
	 	}

		picUpdateCounters(pic_opcode, ca_address1, hit_where1, 
										ca_address2, hit_where2, ca_address3, hit_where3);
		LOG_PRINT("\nL1%d+%lx..+%lx..+%lx, %s,%s,%s", (int)pic_opcode, 
				ca_address1, ca_address2, ca_address3, HitWhereString(hit_where1), 
				HitWhereString(hit_where2), HitWhereString(hit_where3));

	 	//Finally account for PIC operation 1.75 * DATA
	 	//TODO: Make this 1.75
   	getMemoryManager()->incrElapsedTime(m_mem_component, 
				CachePerfModel::ACCESS_CACHE_DATA, ShmemPerfModel::_USER_THREAD);
   	getMemoryManager()->incrElapsedTime(m_mem_component, 
				CachePerfModel::ACCESS_CACHE_DATA, ShmemPerfModel::_USER_THREAD);
   	getMemoryManager()->incrElapsedTime(m_mem_component, 
				CachePerfModel::ACCESS_CACHE_DATA, ShmemPerfModel::_USER_THREAD);
	 	//max hierarchy of the two?
   	return ((hit_where1 > hit_where2) ? hit_where1 : hit_where2);
	}
	else {
			//send to next level, take corrective actions if any
			assert(0);
    	HitWhere::where_t hit_where = HitWhere::UNKNOWN;
      /*getMemoryManager()->incrElapsedTime(m_mem_component, 
				CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);
      getMemoryManager()->incrElapsedTime(m_mem_component, 
			CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);
			picCorrectiveMeasures(pic_opcode, ca_address1, ca_address2);
    	HitWhere::where_t hit_where = 
			m_next_cache_cntlr->processPicReqFromPrevCache(this, pic_opcode, 
				ca_address1, ca_address2, true, true, t_start);*/
			return hit_where;
	}
}

HitWhere::where_t
CacheCntlr::processPicSOpFromCore(
			CacheCntlr::pic_ops_t pic_opcode,
      IntPtr ca_address1, IntPtr ca_address2)
{
	LOG_PRINT("\n%d+%lx..+%lx", (int)pic_opcode, ca_address1, ca_address2);
  SubsecondTime t_start = getShmemPerfModel()->getElapsedTime(
																		ShmemPerfModel::_USER_THREAD);
	//First check if we want to do this operation here
	bool do_operation = 
										pickCurLevelPicOp(pic_opcode, ca_address1, ca_address2);
	if(do_operation) {
		//TODO: Put a switch case for PIC_OPS
		pic_other_load_address = ca_address2;

  	HitWhere::where_t	hit_where1 = picProcessMemOpFromCore(
      Core::NONE, Core::READ,
      ca_address1, 0, NULL, 64, true, true);
  	SubsecondTime t_load_end = getShmemPerfModel()->getElapsedTime
																				(ShmemPerfModel::_USER_THREAD);
		//remember this loaded value
		//Check if during processing of store you throw the loaded value out
		pic_other_load_address = ca_address1;

  	//Start the store at the end of load processing not its *completion*
 		getShmemPerfModel()->setElapsedTime(ShmemPerfModel::_USER_THREAD, 
									t_start) ;
  	getMemoryManager()->incrElapsedTime(m_mem_component, 
			CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);

		HitWhere::where_t hit_where2 = HitWhere::UNKNOWN;
		if(pic_opcode == PIC_COPY) 
  		hit_where2 = picProcessMemOpFromCore(
      Core::NONE, Core::WRITE,
      ca_address2, 0, NULL, 64, true, true);
		else
		//else if(pic_opcode == PIC_CMP)
  		hit_where2 = picProcessMemOpFromCore(
      Core::NONE, Core::READ,
      ca_address2, 0, NULL, 64, true, true);

		pic_other_load_address = 0;

  	SubsecondTime t_store_end = getShmemPerfModel()->getElapsedTime
																				(ShmemPerfModel::_USER_THREAD);
		if(t_load_end > t_store_end)
 			getShmemPerfModel()->setElapsedTime(ShmemPerfModel::_USER_THREAD, 
									t_load_end);

	 	//If any one was a miss, account for more TAG access
	 	if ((hit_where1 != (HitWhere::where_t)m_mem_component) || 
	  	(hit_where2 != (HitWhere::where_t)m_mem_component)) {
      	getMemoryManager()->incrElapsedTime(m_mem_component, 
					CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);
      	getMemoryManager()->incrElapsedTime(m_mem_component, 
				CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);
	 	}

		picUpdateCounters(pic_opcode, ca_address1, hit_where1, 
										ca_address2, hit_where2);
		LOG_PRINT("\nL1%d+%lx..+%lx, %s,%s", (int)pic_opcode, 
				ca_address1, ca_address2, HitWhereString(hit_where1), 
				HitWhereString(hit_where2));

	 	//Finally account for PIC operation 1.75 * DATA
	 	//TODO: Make this 1.75
   	getMemoryManager()->incrElapsedTime(m_mem_component, 
				CachePerfModel::ACCESS_CACHE_DATA, ShmemPerfModel::_USER_THREAD);
   	getMemoryManager()->incrElapsedTime(m_mem_component, 
				CachePerfModel::ACCESS_CACHE_DATA, ShmemPerfModel::_USER_THREAD);
	 	//max hierarchy of the two?
   	return ((hit_where1 > hit_where2) ? hit_where1 : hit_where2);
	}
	else {
			//send to next level, take corrective actions if any
      getMemoryManager()->incrElapsedTime(m_mem_component, 
				CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);
      getMemoryManager()->incrElapsedTime(m_mem_component, 
			CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);
			picCorrectiveMeasures(pic_opcode, ca_address1, ca_address2);
    	HitWhere::where_t hit_where = 
			m_next_cache_cntlr->processPicReqFromPrevCache(this, pic_opcode, 
				ca_address1, ca_address2, true, true, t_start);
			return hit_where;
	}
}

//This is very similar to existing function "processMemOpFromCore" just that I
//avoid updating any statistics and avoid DATA access
HitWhere::where_t
CacheCntlr::picProcessMemOpFromCore(
      Core::lock_signal_t lock_signal,
      Core::mem_op_t mem_op_type,
      IntPtr ca_address, UInt32 offset,
      Byte* data_buf, UInt32 data_length,
      bool modeled,
      bool count)
{  
   //CAP : First check if cache is LLC
   if (!isLastLevel())
   {  // This is to skip the L1 and L2 and restricted accesses to the LLC
      printf("ERROR! Transaction reached L1 or L2");
      assert(0);


   }
   else 
   { 
     HitWhere::where_t hit_where = HitWhere::MISS;
     // Protect against concurrent access from sibling SMT threads
     ScopedLock sl_smt(m_master->m_smt_lock);

     #ifdef PRIVATE_L2_OPTIMIZATION
     if (lock_signal != Core::UNLOCK)
        acquireLock(ca_address);
     #else
     bool lock_all = m_cache_writethrough && ((mem_op_type == Core::WRITE) || (lock_signal != Core::NONE));
     if (lock_signal != Core::UNLOCK) {
        if (lock_all)
           acquireStackLock(ca_address);
        else
           acquireLock(ca_address);
     }
     #endif

     SubsecondTime t_start = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);

     CacheBlockInfo *cache_block_info;
     bool cache_hit = operationPermissibleinCache(ca_address, mem_op_type, 
                                  &cache_block_info);
     if (cache_hit)
     {
        getMemoryManager()->incrElapsedTime(m_mem_component, 
          CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);
        hit_where = (HitWhere::where_t)m_mem_component;

        if (modeled && m_l1_mshr)
        {
           ScopedLock sl(getLock());
           SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(
                                          ShmemPerfModel::_USER_THREAD);
           SubsecondTime t_completed = m_master->m_l1_mshr.getTagCompletionTime(
                                                                      ca_address);
           if (t_completed != SubsecondTime::MaxTime() && t_completed > t_now)
           {
              SubsecondTime latency = t_completed - t_now;
              getShmemPerfModel()->incrElapsedTime(latency, 
                                      ShmemPerfModel::_USER_THREAD);
           }
        }

        if (modeled)
        {
           ScopedLock sl(getLock());
           SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(
                                            ShmemPerfModel::_USER_THREAD);
           if (m_master->mshr.count(ca_address)
              && (m_master->mshr[ca_address].t_issue < t_now && 
                 m_master->mshr[ca_address].t_complete > t_now))
           {
              SubsecondTime latency = m_master->mshr[ca_address].t_complete - 	
                                                                    t_now;
              stats.mshr_latency += latency;
              getMemoryManager()->incrElapsedTime(latency, 
                      ShmemPerfModel::_USER_THREAD);
           }
        }

     } else {
        //We don't access DATA
        getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);

        SubsecondTime t_miss_begin = getShmemPerfModel()->getElapsedTime(
                                          ShmemPerfModel::_USER_THREAD);
        SubsecondTime t_mshr_avail = t_miss_begin;
        if (modeled && m_l1_mshr)
        {
           ScopedLock sl(getLock());
           t_mshr_avail = m_master->m_l1_mshr.getStartTime(t_miss_begin);
           SubsecondTime mshr_latency = t_mshr_avail - t_miss_begin;
           // Delay until we have an empty slot in the MSHR
           getShmemPerfModel()->incrElapsedTime(mshr_latency, 
                                    ShmemPerfModel::_USER_THREAD);
           stats.mshr_latency += mshr_latency;
        }

        #ifdef PRIVATE_L2_OPTIMIZATION
        #else
        if (!lock_all)
           acquireStackLock(ca_address, true);
        #endif

        // Invalidate the cache block before passing the request to L2 Cache
        if (getCacheState(ca_address) != CacheState::INVALID) {
           invalidateCacheBlock(ca_address);
        }

        hit_where = m_next_cache_cntlr->processShmemReqFromPrevCache(this, mem_op_type, ca_address, modeled, count, Prefetch::NONE, t_start, false,
        pic_other_load_address, pic_other_load2_address);
        bool next_cache_hit = hit_where != HitWhere::MISS;
        if (next_cache_hit) {

        } else {
           #ifdef PRIVATE_L2_OPTIMIZATION
           releaseLock(ca_address);
           #else
           releaseStackLock(ca_address);
           #endif
           waitForNetworkThread();
           wakeUpNetworkThread();
           hit_where = m_next_cache_cntlr->processShmemReqFromPrevCache(this, mem_op_type, ca_address, false, false, Prefetch::NONE, t_start,true,
            pic_other_load_address, pic_other_load2_address);
           #ifdef PRIVATE_L2_OPTIMIZATION
           releaseStackLock(ca_address, true);
           #else
           #endif
        }
        SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(
                                              ShmemPerfModel::_USER_THREAD);
        copyDataFromNextLevel(mem_op_type, ca_address, modeled, t_now,
        pic_other_load_address, pic_other_load2_address);
        cache_block_info = getCacheBlockInfo(ca_address);

        #ifdef PRIVATE_L2_OPTIMIZATION
        #else
        if (!lock_all)
           releaseStackLock(ca_address, true);
        #endif

        if (modeled && m_l1_mshr) {
           SubsecondTime t_miss_end = getShmemPerfModel()->getElapsedTime(
                                        ShmemPerfModel::_USER_THREAD);
           ScopedLock sl(getLock());
           m_master->m_l1_mshr.getCompletionTime(t_miss_begin, 
                              t_miss_end - t_mshr_avail, ca_address);
        }
     }


     accessCache(mem_op_type, ca_address, offset, data_buf, data_length, 
                hit_where == HitWhere::where_t(m_mem_component) && count);

     // From here on downwards: not long anymore, only stats update so blanket cntrl lock
     {
        ScopedLock sl(getLock());

        /* if this is the first part of an atomic operation: keep the lock(s) */
        #ifdef PRIVATE_L2_OPTIMIZATION
        if (lock_signal != Core::LOCK)
           releaseLock(ca_address);
        #else
        if (lock_signal != Core::LOCK) {
           if (lock_all)
              releaseStackLock(ca_address);
           else
              releaseLock(ca_address);
        }
        #endif
     }
     return hit_where;  
   }
}

void
CacheCntlr::initiatePicDirectoryAccess(CacheCntlr::pic_ops_t pic_opcode, 
IntPtr address1, IntPtr address2, SubsecondTime t_issue ) {
   bool exclusive = false;
   switch (pic_opcode) {
      case CacheCntlr::PIC_COPY:
         exclusive = true;
         break;
      case CacheCntlr::PIC_CMP:
      case CacheCntlr::PIC_SEARCH:
         exclusive = false;
         break;
      default:
         LOG_PRINT_ERROR("Unsupported Pic Type(%u)", pic_opcode);
   }
   bool first = false;
   {
      ScopedLock sl(getLock());
			//Use store address
      CacheDirectoryWaiter* request = 
				new CacheDirectoryWaiter(exclusive, false, this, t_issue);
      m_master->m_directory_waiters.enqueue(address2, request);
			//I dont want other request pending for now
      assert(m_master->m_directory_waiters.size(address2) == 1);
      first = true;
   }

   if (first) {
      m_shmem_perf->reset(getShmemPerfModel()->getElapsedTime
				(ShmemPerfModel::_USER_THREAD));
      if (exclusive) {
   			getMemoryManager()->sendMsg(
				 PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_COPY_REQ,
         MemComponent::LAST_LEVEL_CACHE, MemComponent::TAG_DIR,
         m_core_id_master /* requester */,
         getHome(address2) /* receiver */,
         address2,
         NULL, 0,
         HitWhere::UNKNOWN, m_shmem_perf, ShmemPerfModel::_USER_THREAD,
				 address1);
      }
      else
      {
   			getMemoryManager()->sendMsg(
				 pic_opcode == CacheCntlr::PIC_CMP ? 
				 PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_CMP_REQ :
				 PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_SEARCH_REQ,
         MemComponent::LAST_LEVEL_CACHE, MemComponent::TAG_DIR,
         m_core_id_master /* requester */,
         getHome(address2) /* receiver */,
         address2,
         NULL, 0,
         HitWhere::UNKNOWN, m_shmem_perf, ShmemPerfModel::_USER_THREAD,
				 address1);
      }
   }
}

HitWhere::where_t
CacheCntlr::processMemOpFromCore(
      Core::lock_signal_t lock_signal,
      Core::mem_op_t mem_op_type,
      IntPtr ca_address, UInt32 offset,
      Byte* data_buf, UInt32 data_length,
      bool modeled,
      bool count)
{
   HitWhere::where_t hit_where = HitWhere::MISS;

   // Protect against concurrent access from sibling SMT threads
   ScopedLock sl_smt(m_master->m_smt_lock);

   LOG_PRINT("processMemOpFromCore(), lock_signal(%u), mem_op_type(%u), ca_address(0x%x)",
             lock_signal, mem_op_type, ca_address);
MYLOG("----------------------------------------------");
MYLOG("%c%c %lx+%u..+%u", mem_op_type == Core::WRITE ? 'W' : 'R', mem_op_type == Core::READ_EX ? 'X' : ' ', ca_address, offset, data_length);

if(DEBUG_ENABLED)  printf("\n processMemOpFromCore(), mem_op_type(%u), ca_address(0x%x), data(%d) data length(%d), offset(%d)", mem_op_type, ca_address, (UInt32)(*data_buf), data_length, offset);



LOG_ASSERT_ERROR((ca_address & (getCacheBlockSize() - 1)) == 0, "address at cache line + %x", ca_address & (getCacheBlockSize() - 1));
LOG_ASSERT_ERROR(offset + data_length <= getCacheBlockSize(), "access until %u > %u", offset + data_length, getCacheBlockSize());

   #ifdef PRIVATE_L2_OPTIMIZATION
   /* if this is the second part of an atomic operation: we already have the lock, don't lock again */
   if (lock_signal != Core::UNLOCK)
      acquireLock(ca_address);
   #else
   /* if we'll need the next level (because we're a writethrough cache, and either this is a write
      or we're part of an atomic pair in which this or the other memop is potentially a write):
      make sure to lock it now, so the cache line in L2 doesn't fall from under us
      between operationPermissibleinCache and the writethrough */
   bool lock_all = m_cache_writethrough && ((mem_op_type == Core::WRITE) || (lock_signal != Core::NONE));

    /* if this is the second part of an atomic operation: we already have the lock, don't lock again */
   if (lock_signal != Core::UNLOCK) {
      if (lock_all)
         acquireStackLock(ca_address);
      else
         acquireLock(ca_address);
   }
   #endif

   SubsecondTime t_start = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);

   CacheBlockInfo *cache_block_info;
   bool cache_hit = operationPermissibleinCache(ca_address, mem_op_type, &cache_block_info), prefetch_hit = false;
   if (mem_op_type == Core::WRITE) {
		 m_next_cache_cntlr->addLatencyMshrPic(ca_address);
   }

   if (!cache_hit && m_perfect)
   {
      cache_hit = true;
      hit_where = HitWhere::where_t(m_mem_component);
      if (cache_block_info)
         cache_block_info->setCState(CacheState::MODIFIED);
      else
      {
         insertCacheBlock(ca_address, mem_op_type == Core::READ ? CacheState::SHARED : CacheState::MODIFIED, NULL, m_core_id, ShmemPerfModel::_USER_THREAD);
         cache_block_info = getCacheBlockInfo(ca_address);
      }
   }

   if (count)
   {
      ScopedLock sl(getLock());
      // Update the Cache Counters
      getCache()->updateCounters(cache_hit);
      updateCounters(mem_op_type, ca_address, cache_hit, getCacheState(cache_block_info), Prefetch::NONE);
   }

   if (cache_hit)
   {
MYLOG("L1 hit");
      getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS, ShmemPerfModel::_USER_THREAD);
      hit_where = (HitWhere::where_t)m_mem_component;

      if (cache_block_info->hasOption(CacheBlockInfo::WARMUP) && Sim()->getInstrumentationMode() != InstMode::CACHE_ONLY)
      {
         stats.hits_warmup++;
         cache_block_info->clearOption(CacheBlockInfo::WARMUP);
      }
      if (cache_block_info->hasOption(CacheBlockInfo::PREFETCH))
      {
         // This line was fetched by the prefetcher and has proven useful
         stats.hits_prefetch++;
         prefetch_hit = true;
         cache_block_info->clearOption(CacheBlockInfo::PREFETCH);
      }

      if (modeled && m_l1_mshr)
      {
         ScopedLock sl(getLock());
         SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
         SubsecondTime t_completed = m_master->m_l1_mshr.getTagCompletionTime(ca_address);
         if (t_completed != SubsecondTime::MaxTime() && t_completed > t_now)
         {
            if (mem_op_type == Core::WRITE)
               ++stats.store_overlapping_misses;
            else
               ++stats.load_overlapping_misses;

            SubsecondTime latency = t_completed - t_now;
            getShmemPerfModel()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
         }
      }

      if (modeled)
      {
         ScopedLock sl(getLock());
         // This is a hit, but maybe the prefetcher filled it at a future time stamp. If so, delay.
         SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
         if (m_master->mshr.count(ca_address)
            && (m_master->mshr[ca_address].t_issue < t_now && m_master->mshr[ca_address].t_complete > t_now))
         {
            SubsecondTime latency = m_master->mshr[ca_address].t_complete - t_now;
            stats.mshr_latency += latency;
            getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
         }
      }

   } else {
      /* cache miss: either wrong coherency state or not present in the cache */
MYLOG("L1 miss");
      getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);

      SubsecondTime t_miss_begin = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
      SubsecondTime t_mshr_avail = t_miss_begin;
      if (modeled && m_l1_mshr)
      {
         ScopedLock sl(getLock());
         t_mshr_avail = m_master->m_l1_mshr.getStartTime(t_miss_begin);
         LOG_ASSERT_ERROR(t_mshr_avail >= t_miss_begin, "t_mshr_avail < t_miss_begin");
         SubsecondTime mshr_latency = t_mshr_avail - t_miss_begin;
         // Delay until we have an empty slot in the MSHR
         getShmemPerfModel()->incrElapsedTime(mshr_latency, ShmemPerfModel::_USER_THREAD);
         stats.mshr_latency += mshr_latency;
      }

      if (lock_signal == Core::UNLOCK)
         LOG_PRINT_ERROR("Expected to find address(0x%x) in L1 Cache", ca_address);

      #ifdef PRIVATE_L2_OPTIMIZATION
      #else
      if (!lock_all)
         acquireStackLock(ca_address, true);
      #endif

      // Invalidate the cache block before passing the request to L2 Cache
      if (getCacheState(ca_address) != CacheState::INVALID)
      {
         invalidateCacheBlock(ca_address);
      }

MYLOG("processMemOpFromCore l%d before next", m_mem_component);
      hit_where = m_next_cache_cntlr->processShmemReqFromPrevCache(this, mem_op_type, ca_address, modeled, count, Prefetch::NONE, t_start, false);
      bool next_cache_hit = hit_where != HitWhere::MISS;
MYLOG("processMemOpFromCore l%d next hit = %d", m_mem_component, next_cache_hit);

      if (next_cache_hit) {

      } else {
         /* last level miss, a message has been sent. */

MYLOG("processMemOpFromCore l%d waiting for sent message", m_mem_component);
         #ifdef PRIVATE_L2_OPTIMIZATION
         releaseLock(ca_address);
         #else
         releaseStackLock(ca_address);
         #endif

         waitForNetworkThread();
MYLOG("processMemOpFromCore l%d postwakeup", m_mem_component);

         //acquireStackLock(ca_address);
         // Pass stack lock through from network thread

         wakeUpNetworkThread();
MYLOG("processMemOpFromCore l%d got message reply", m_mem_component);

         /* have the next cache levels fill themselves with the new data */
MYLOG("processMemOpFromCore l%d before next fill", m_mem_component);
         hit_where = m_next_cache_cntlr->processShmemReqFromPrevCache(this, mem_op_type, ca_address, false, false, Prefetch::NONE, t_start, true);

MYLOG("processMemOpFromCore l%d after next fill", m_mem_component);
         LOG_ASSERT_ERROR(hit_where != HitWhere::MISS,
            "Tried to read in next-level cache, but data is already gone");

         #ifdef PRIVATE_L2_OPTIMIZATION
         releaseStackLock(ca_address, true);
         #else
         #endif
      }


      /* data should now be in next-level cache, go get it */
      SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
      copyDataFromNextLevel(mem_op_type, ca_address, modeled, t_now);

      cache_block_info = getCacheBlockInfo(ca_address);

      #ifdef PRIVATE_L2_OPTIMIZATION
      #else
      if (!lock_all)
         releaseStackLock(ca_address, true);
      #endif

      LOG_ASSERT_ERROR(operationPermissibleinCache(ca_address, mem_op_type),
         "Expected %x to be valid in L1", ca_address);


      if (modeled && m_l1_mshr)
      {
         SubsecondTime t_miss_end = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
         ScopedLock sl(getLock());
         m_master->m_l1_mshr.getCompletionTime(t_miss_begin, t_miss_end - t_mshr_avail, ca_address);
      }
   }


   if (modeled && m_next_cache_cntlr && !m_perfect && Sim()->getConfig()->hasCacheEfficiencyCallbacks())
   {
      bool new_bits = cache_block_info->updateUsage(offset, data_length);
      if (new_bits)
      {
         m_next_cache_cntlr->updateUsageBits(ca_address, cache_block_info->getUsage());
      }
   }


   accessCache(mem_op_type, ca_address, offset, data_buf, data_length, hit_where == HitWhere::where_t(m_mem_component) && count);
MYLOG("access done");


   SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
   SubsecondTime total_latency = t_now - t_start;

   // From here on downwards: not long anymore, only stats update so blanket cntrl lock
   {
      ScopedLock sl(getLock());

      if (! cache_hit && count) {
         stats.total_latency += total_latency;
      }

      #ifdef TRACK_LATENCY_BY_HITWHERE
      if (count)
         lat_by_where[hit_where].update(total_latency.getNS());
      #endif

      /* if this is the first part of an atomic operation: keep the lock(s) */
      #ifdef PRIVATE_L2_OPTIMIZATION
      if (lock_signal != Core::LOCK)
         releaseLock(ca_address);
      #else
      if (lock_signal != Core::LOCK) {
         if (lock_all)
            releaseStackLock(ca_address);
         else
            releaseLock(ca_address);
      }
      #endif

      if (mem_op_type == Core::WRITE)
         stats.stores_where[hit_where]++;
      else
         stats.loads_where[hit_where]++;
   }


   if (modeled && m_master->m_prefetcher)
   {
      trainPrefetcher(ca_address, cache_hit, prefetch_hit, t_start);
   }

   // Call Prefetch on next-level caches (but not for atomic instructions as that causes a locking mess)
   if (lock_signal != Core::LOCK && modeled)
   {
      Prefetch(t_start);
   }

   if (Sim()->getConfig()->getCacheEfficiencyCallbacks().notify_access_func)
      Sim()->getConfig()->getCacheEfficiencyCallbacks().call_notify_access(cache_block_info->getOwner(), mem_op_type, hit_where);

   MYLOG("returning %s, latency %lu ns", HitWhereString(hit_where), total_latency.getNS());
   return hit_where;
}


void
CacheCntlr::updateHits(Core::mem_op_t mem_op_type, UInt64 hits)
{
   ScopedLock sl(getLock());

   while(hits > 0)
   {
      getCache()->updateCounters(true);
      updateCounters(mem_op_type, 0, true, mem_op_type == Core::READ ? CacheState::SHARED : CacheState::MODIFIED, Prefetch::NONE);
      hits--;
   }
}


void
CacheCntlr::copyDataFromNextLevel(Core::mem_op_t mem_op_type, IntPtr address, bool modeled, SubsecondTime t_now, IntPtr other_pic_address
,IntPtr other_pic_address2)
{
   // TODO: what if it's already gone? someone else may invalitate it between the time it arrived an when we get here...
   LOG_ASSERT_ERROR(m_next_cache_cntlr->operationPermissibleinCache(address, mem_op_type),
      "Tried to read from next-level cache, but data is already gone");
MYLOG("copyDataFromNextLevel l%d", m_mem_component);

   Byte data_buf[m_next_cache_cntlr->getCacheBlockSize()];
   m_next_cache_cntlr->retrieveCacheBlock(address, data_buf, ShmemPerfModel::_USER_THREAD, false);

   CacheState::cstate_t cstate = m_next_cache_cntlr->getCacheState(address);

   // TODO: increment time? tag access on next level, also data access if this is not an upgrade

   if (modeled && !m_next_level_read_bandwidth.isInfinite())
   {
      SubsecondTime delay = m_next_level_read_bandwidth.getRoundedLatency(getCacheBlockSize() * 8);
      SubsecondTime t_done = m_master->m_next_level_read_bandwidth.getCompletionTime(t_now, delay);
      // Assume cache access time already contains transfer latency, increment time by contention delay only
      LOG_ASSERT_ERROR(t_done >= t_now + delay, "Did not expect next-level cache to be this fast");
      getMemoryManager()->incrElapsedTime(t_done - t_now - delay, ShmemPerfModel::_USER_THREAD);
   }

   SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);
   if (cache_block_info)
   {
      // Block already present (upgrade): don't insert, but update
      updateCacheBlock(address, cstate, Transition::UPGRADE, NULL, ShmemPerfModel::_SIM_THREAD);
      MYLOG("copyDataFromNextLevel l%d done (updated)", m_mem_component);
   }
   else
   {
      // Insert the Cache Block in our own cache
      insertCacheBlock(address, cstate, data_buf, m_core_id, ShmemPerfModel::_USER_THREAD, other_pic_address, other_pic_address2);
      MYLOG("copyDataFromNextLevel l%d done (inserted)", m_mem_component);
   }
}


void
CacheCntlr::trainPrefetcher(IntPtr address, bool cache_hit, bool prefetch_hit, SubsecondTime t_issue)
{
   ScopedLock sl(getLock());

   // Always train the prefetcher
   std::vector<IntPtr> prefetchList = m_master->m_prefetcher->getNextAddress(address, m_core_id);

   // Only do prefetches on misses, or on hits to lines previously brought in by the prefetcher (if enabled)
   if (!cache_hit || (m_prefetch_on_prefetch_hit && prefetch_hit))
   {
      m_master->m_prefetch_list.clear();
      // Just talked to the next-level cache, wait a bit before we start to prefetch
      m_master->m_prefetch_next = t_issue + PREFETCH_INTERVAL;

      for(std::vector<IntPtr>::iterator it = prefetchList.begin(); it != prefetchList.end(); ++it)
      {
         // Keep at most PREFETCH_MAX_QUEUE_LENGTH entries in the prefetch queue
         if (m_master->m_prefetch_list.size() > PREFETCH_MAX_QUEUE_LENGTH)
            break;
         if (!operationPermissibleinCache(*it, Core::READ))
            m_master->m_prefetch_list.push_back(*it);
      }
   }
}

void
CacheCntlr::Prefetch(SubsecondTime t_now)
{
   IntPtr address_to_prefetch = INVALID_ADDRESS;

   {
      ScopedLock sl(getLock());

      if (m_master->m_prefetch_next <= t_now)
      {
         while(!m_master->m_prefetch_list.empty())
         {
            IntPtr address = m_master->m_prefetch_list.front();
            m_master->m_prefetch_list.pop_front();

            // Check address again, maybe some other core already brought it into the cache
            if (!operationPermissibleinCache(address, Core::READ))
            {
               address_to_prefetch = address;
               // Do at most one prefetch now, save the rest for a future call
               break;
            }
         }
      }
   }

   if (address_to_prefetch != INVALID_ADDRESS)
   {
      doPrefetch(address_to_prefetch, m_master->m_prefetch_next);
      atomic_add_subsecondtime(m_master->m_prefetch_next, PREFETCH_INTERVAL);
   }

   // In case the next-level cache has a prefetcher, run it
   if (m_next_cache_cntlr)
      m_next_cache_cntlr->Prefetch(t_now);
}

void
CacheCntlr::doPrefetch(IntPtr prefetch_address, SubsecondTime t_start)
{
   ++stats.prefetches;
   acquireStackLock(prefetch_address);
   MYLOG("prefetching %lx", prefetch_address);
   SubsecondTime t_before = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
   getShmemPerfModel()->setElapsedTime(ShmemPerfModel::_USER_THREAD, t_start); // Start the prefetch at the same time as the original miss
   HitWhere::where_t hit_where = processShmemReqFromPrevCache(this, Core::READ, prefetch_address, true, true, Prefetch::OWN, t_start, false);

   if (hit_where == HitWhere::MISS)
   {
      /* last level miss, a message has been sent. */

      releaseStackLock(prefetch_address);
      waitForNetworkThread();
      wakeUpNetworkThread();

      hit_where = processShmemReqFromPrevCache(this, Core::READ, prefetch_address, false, false, Prefetch::OWN, t_start, false);

      LOG_ASSERT_ERROR(hit_where != HitWhere::MISS, "Line was not there after prefetch");
   }

   getShmemPerfModel()->setElapsedTime(ShmemPerfModel::_USER_THREAD, t_before); // Ignore changes to time made by the prefetch call
   releaseStackLock(prefetch_address);
}


/*****************************************************************************
 * operations called by cache on next-level cache
 *****************************************************************************/
//We are sending a pic op to next level, take corrective measures
void CacheCntlr::picCorrectiveMeasures(
CacheCntlr::pic_ops_t pic_opcode, IntPtr ca_address1, IntPtr ca_address2) {
	ShmemPerfModel::Thread_t thread_num = ShmemPerfModel::_USER_THREAD;	
	int num_wb 	= 0;
	int num_inv	= 0;
	//1. ca_address1 has to be written to next level
	if (getCacheState(ca_address1) == CacheState::MODIFIED) {
  	Byte data_buf[getCacheBlockSize()];
   	retrieveCacheBlock(ca_address1, data_buf, thread_num, false);
		if(m_next_cache_cntlr) {
  		m_next_cache_cntlr->writeCacheBlock(ca_address1, 0, data_buf, 
												getCacheBlockSize(), thread_num, true);
		}
		else {
      UInt32 home_node_id = getHome(ca_address1);
      getMemoryManager()->sendMsg(
						PrL1PrL2DramDirectoryMSI::ShmemMsg::FLUSH_DATA_REP,
            MemComponent::LAST_LEVEL_CACHE, MemComponent::TAG_DIR,
            m_core_id /* requester */,
            home_node_id /* receiver */,
            ca_address1,
            data_buf, getCacheBlockSize(),
            HitWhere::UNKNOWN, 
						NULL, thread_num);
   	}
		++num_wb;
	}

	//2. ca_address2, writeback if modified || invalide for copy
	if(pic_opcode == PIC_COPY) {  
		//TODO: Will throw an error
		//Typically not ok to invalidate modified data, ok for copy
		if (getCacheState(ca_address2) != CacheState::INVALID) {
  		invalidateCacheBlock(ca_address2);
			++num_inv;
		}
  }
	else {
		if((getCacheState(ca_address2) == CacheState::MODIFIED)) {
  		Byte data_buf[getCacheBlockSize()];
   		retrieveCacheBlock(ca_address2, data_buf, thread_num, false);
			if (m_next_cache_cntlr) {
  			m_next_cache_cntlr->writeCacheBlock(ca_address2, 0, data_buf, 
												getCacheBlockSize(), thread_num, true);
			}
			else {
        UInt32 home_node_id = getHome(ca_address2);
      	getMemoryManager()->sendMsg(
						PrL1PrL2DramDirectoryMSI::ShmemMsg::FLUSH_DATA_REP,
            MemComponent::LAST_LEVEL_CACHE, MemComponent::TAG_DIR,
            m_core_id /* requester */,
            home_node_id /* receiver */,
            ca_address2,
            data_buf, getCacheBlockSize(),
            HitWhere::UNKNOWN, 
						NULL, thread_num);
   		}
			++num_wb;
		}
	}
	picUpdateCorrections(pic_opcode, num_inv, num_wb);
}

HitWhere::where_t
CacheCntlr::picProcessShmemReqFromPrevCache(CacheCntlr* requester, 
	Core::mem_op_t mem_op_type, IntPtr address, bool modeled, bool count, 
	SubsecondTime t_issue) {
   #ifdef PRIVATE_L2_OPTIMIZATION
   if ( m_shared_cores > 1) {
   		acquireStackLock(address);
   }
   #else
   	acquireStackLock(address);
   #endif
	bool cache_hit = operationPermissibleinCache(address, mem_op_type), 
																		sibling_hit = false;
  bool first_hit = cache_hit;
  HitWhere::where_t hit_where = HitWhere::MISS;
  SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);

	//Hit or miss, pretend a tag check happened
  getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);
  if (cache_hit) {
  	if (modeled) {
    	ScopedLock sl(getLock());
      // This is a hit, but maybe the prefetcher filled it at a future time stamp. If so, delay.
      SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(
																		ShmemPerfModel::_USER_THREAD);
      if (m_master->mshr.count(address)
      	&& (m_master->mshr[address].t_issue < t_now && 
														m_master->mshr[address].t_complete > t_now)) {
      	SubsecondTime latency = m_master->mshr[address].t_complete - t_now;
        stats.mshr_latency += latency;
        getMemoryManager()->incrElapsedTime(latency, 
															ShmemPerfModel::_USER_THREAD);
			}
    }

		//TODO: Why no tag+data here?

    if (mem_op_type != Core::READ) { // write that hits
    	/* Invalidate/flush in previous levels */
      SubsecondTime latency = SubsecondTime::Zero();
      for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin();
					it != m_master->m_prev_cache_cntlrs.end(); it++) {
      	if (*it != requester) {
        	std::pair<SubsecondTime, bool> res = 
						(*it)->updateCacheBlock(address, CacheState::INVALID, 
						Transition::COHERENCY, NULL, ShmemPerfModel::_USER_THREAD);
            latency = getMax<SubsecondTime>(latency, res.first);
            sibling_hit |= res.second;
        }
      }
      getMemoryManager()->incrElapsedTime(latency, 
																				ShmemPerfModel::_USER_THREAD);
      atomic_add_subsecondtime(stats.snoop_latency, latency);
      #ifdef ENABLE_TRACK_SHARING_PREVCACHES
      	assert(! cache_block_info->hasCachedLoc());
      #endif
		}
    else if (cache_block_info->getCState() == CacheState::MODIFIED) {
			// reading MODIFIED data
      MYLOG("reading MODIFIED data");
      /* Writeback in previous levels */
      SubsecondTime latency = SubsecondTime::Zero();
      for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); 				it != m_master->m_prev_cache_cntlrs.end(); it++) {
        if (*it != requester) {
        	std::pair<SubsecondTime, bool> res = 
						(*it)->updateCacheBlock(address, CacheState::SHARED, 
							Transition::COHERENCY, NULL, ShmemPerfModel::_USER_THREAD);
          latency = getMax<SubsecondTime>(latency, res.first);
          sibling_hit |= res.second;
        }
			}
      getMemoryManager()->incrElapsedTime(latency, 
																ShmemPerfModel::_USER_THREAD);
      atomic_add_subsecondtime(stats.snoop_latency, latency);
		}
    else if (cache_block_info->getCState() == CacheState::EXCLUSIVE) { 
			// reading EXCLUSIVE data
      MYLOG("reading EXCLUSIVE data");
      // will have shared state
      SubsecondTime latency = SubsecondTime::Zero();
      for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); 				it != m_master->m_prev_cache_cntlrs.end(); it++) {
        if (*it != requester) {
        	std::pair<SubsecondTime, bool> res = 
						(*it)->updateCacheBlock(address, CacheState::SHARED, 
						Transition::COHERENCY, NULL, ShmemPerfModel::_USER_THREAD);
          latency = getMax<SubsecondTime>(latency, res.first);
          sibling_hit |= res.second;
        }
      }

      getMemoryManager()->incrElapsedTime(latency, 
																	ShmemPerfModel::_USER_THREAD);
      atomic_add_subsecondtime(stats.snoop_latency, latency);
    }

		//TODO: This fails.. Can network thread update this?
		//assert(m_last_remote_hit_where == HitWhere::UNKNOWN);
    hit_where = HitWhere::where_t(m_mem_component + (sibling_hit ? 
																						HitWhere::SIBLING : 0));
   	}
   else {
		// !cache_hit: either data is not here, 
		//or operation on data is not permitted
    // Increment shared mem perf model cycle counts
      if (cache_block_info && (cache_block_info->getCState() == 
																									CacheState::SHARED)){
      	// Data is present, but still no cache_hit => this is a write on a SHARED block. Do Upgrade
        SubsecondTime latency = SubsecondTime::Zero();
        for(CacheCntlrList::iterator it = 
					m_master->m_prev_cache_cntlrs.begin(); 
					it != m_master->m_prev_cache_cntlrs.end(); it++)
          if (*it != requester)
          	latency = getMax<SubsecondTime>(latency, 
							(*it)->updateCacheBlock(address, CacheState::INVALID, 
							Transition::UPGRADE, NULL, ShmemPerfModel::_USER_THREAD).first);
         		getMemoryManager()->incrElapsedTime(latency, 
																								ShmemPerfModel::_USER_THREAD);
         	atomic_add_subsecondtime(stats.snoop_latency, latency);
         	#ifdef ENABLE_TRACK_SHARING_PREVCACHES
         		assert(! cache_block_info->hasCachedLoc());
         	#endif
			}
      if (m_next_cache_cntlr) {
      	if (cache_block_info)
        	invalidateCacheBlock(address);

        // let the next cache level handle it.
        hit_where = m_next_cache_cntlr->processShmemReqFromPrevCache(this, 
										mem_op_type, address, modeled, count, 
										Prefetch::NONE, t_issue, true, pic_other_load_address, pic_other_load2_address);
        SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(
																	ShmemPerfModel::_USER_THREAD);
        if (hit_where != HitWhere::MISS) {
        	cache_hit = true;
          /* get the data for ourselves */
          copyDataFromNextLevel(mem_op_type, address, modeled, t_now);
        }
				else {
					//Typically happens in L1, but we are crazily bypassing it
      		releaseStackLock(address);
      		waitForNetworkThread();
      		wakeUpNetworkThread();
					if(mem_op_type == Core::WRITE) 
						m_next_cache_cntlr->getCacheBlockInfo(address)->setCState(CacheState::MODIFIED);
          copyDataFromNextLevel(mem_op_type, address, modeled, t_now);
					assert(0);
				}
      }
      else { // last-level cache
      	if (cache_block_info && cache_block_info->getCState() == 
																								CacheState::EXCLUSIVE) {
        	// Data is present, but still no cache_hit => this is a write on 
					// a SHARED block. Do Upgrade
          SubsecondTime latency = SubsecondTime::Zero();
          for(CacheCntlrList::iterator it = 
						m_master->m_prev_cache_cntlrs.begin(); 
						it != m_master->m_prev_cache_cntlrs.end(); it++)
            if (*it != requester)
            	latency = getMax<SubsecondTime>(latency, 
								(*it)->updateCacheBlock(address, CacheState::INVALID,
								Transition::UPGRADE, NULL, 
								ShmemPerfModel::_USER_THREAD).first);
          getMemoryManager()->incrElapsedTime(latency, 
						ShmemPerfModel::_USER_THREAD);
          atomic_add_subsecondtime(stats.snoop_latency, latency);
          #ifdef ENABLE_TRACK_SHARING_PREVCACHES
          	assert(! cache_block_info->hasCachedLoc());
          #endif
          cache_hit = true;
          hit_where = HitWhere::where_t(m_mem_component);
          cache_block_info->setCState(CacheState::MODIFIED);
				}
        else if (m_master->m_dram_cntlr) {
        	// Direct DRAM access
          cache_hit = true;
          if (cache_block_info) {
          	// We already have the line: it must have been SHARED and this is a write (else there wouldn't have been a miss)
            // Upgrade silently
            cache_block_info->setCState(CacheState::MODIFIED);
            hit_where = HitWhere::where_t(m_mem_component);
          }
          else {
            Byte data_buf[getCacheBlockSize()];
            SubsecondTime latency;
						bool dram_accessed =  false;
            // Do the DRAM access and increment local time
						if(!m_pic_avoid_dram || !(mem_op_type == Core::WRITE))
							{
							  dram_accessed =  true;
          			m_shmem_perf->reset(getShmemPerfModel()->getElapsedTime(
								ShmemPerfModel::_USER_THREAD));
            		boost::tie<HitWhere::where_t, SubsecondTime>(hit_where, 
																																			latency) 
									= accessDRAM(Core::READ, address, false, data_buf);
            		getMemoryManager()->incrElapsedTime(latency, 
									ShmemPerfModel::_USER_THREAD);
							}
						if(m_pic_avoid_dram && (mem_op_type == Core::WRITE))
							hit_where = HitWhere::DRAM;
            // Insert the line. Be sure to use SHARED/MODIFIED as appropriate
						// (upgrades are free anyway), we don't want to have to write 
						// back clean lines
            insertCacheBlock(address, mem_op_type == Core::READ ? 
							CacheState::SHARED : CacheState::MODIFIED, data_buf, m_core_id,
							ShmemPerfModel::_USER_THREAD, pic_other_load_address, pic_other_load2_address);
            
						if(dram_accessed)
							updateUncoreStatistics(HitWhere::UNKNOWN, 
								getShmemPerfModel()->getElapsedTime(
								ShmemPerfModel::_USER_THREAD));
          }
       	}
        else {
        	initiateDirectoryAccess(mem_op_type, address, false, t_issue, 
						pic_other_load_address, pic_other_load2_address);
      		releaseStackLock(address);
					//Typically happens in L1, but we are crazily bypassing it
      		waitForNetworkThread();
      		wakeUpNetworkThread();
					assert(m_last_remote_hit_where != HitWhere::UNKNOWN);
					hit_where = m_last_remote_hit_where;
					m_last_remote_hit_where = HitWhere::UNKNOWN;
					if(mem_op_type == Core::WRITE) //Hacking
						getCacheBlockInfo(address)->setCState(CacheState::MODIFIED);
        }
      }
   }
   if (cache_hit)
   {
      MYLOG("Yay, hit!!");
      Byte data_buf[getCacheBlockSize()];
      retrieveCacheBlock(address, data_buf, ShmemPerfModel::_USER_THREAD, first_hit && count);
      /* Store completion time so we can detect overlapping accesses */
      if (modeled && !first_hit)
      {
         ScopedLock sl(getLock());
         m_master->mshr[address] = make_mshr(t_issue, getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD));
         cleanupMshr();
      }
   }

   #ifdef PRIVATE_L2_OPTIMIZATION
   if (m_shared_cores > 1) {
   	releaseStackLock(address);
   }
   #else
   	releaseStackLock(address);
   #endif
   return hit_where;
}

HitWhere::where_t
CacheCntlr::processPicReqFromPrevCache(CacheCntlr* requester, 
CacheCntlr::pic_ops_t pic_opcode, IntPtr ca_address1, IntPtr ca_address2,
bool modeled, bool count, SubsecondTime t_issue) {

	LOG_PRINT("processPicReqFromPrevCache l%d pic(%d), (%lx, %lx)", m_mem_component, (int) pic_opcode, ca_address1, ca_address2);
	//First check if we want to do this operation here
	bool do_operation = 
										pickCurLevelPicOp(pic_opcode, ca_address1, ca_address2);
	if(do_operation) {
  	SubsecondTime t_start = getShmemPerfModel()->getElapsedTime(
																		ShmemPerfModel::_USER_THREAD);
		HitWhere::where_t hit_where1 = HitWhere::UNKNOWN;
		HitWhere::where_t hit_where2 = HitWhere::UNKNOWN;
		pic_other_load_address = ca_address2;

		hit_where1 = picProcessShmemReqFromPrevCache(requester, 
									Core::READ, ca_address1, modeled, count,
									t_issue);
  	SubsecondTime t_first_end = getShmemPerfModel()->getElapsedTime
																				(ShmemPerfModel::_USER_THREAD);
		pic_other_load_address = ca_address1;

  	//Start the store at the end of load processing not its *completion*
 		getShmemPerfModel()->setElapsedTime(ShmemPerfModel::_USER_THREAD, 
									t_start) ;
  	getMemoryManager()->incrElapsedTime(m_mem_component, 
			CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);

		if(pic_opcode == PIC_COPY) 
			hit_where2 	= picProcessShmemReqFromPrevCache(requester, Core::WRITE, 
			ca_address2, modeled, count, t_issue);
		//else if(pic_opcode == PIC_CMP) 
		else
			hit_where2	= picProcessShmemReqFromPrevCache(requester, Core::READ, 
			ca_address2, modeled, count, t_issue);
  	SubsecondTime t_second_end = getShmemPerfModel()->getElapsedTime
																				(ShmemPerfModel::_USER_THREAD);
		if(t_first_end > t_second_end)
 			getShmemPerfModel()->setElapsedTime(ShmemPerfModel::_USER_THREAD, 
									t_first_end);

		pic_other_load_address = 0;
	 	//If any one was a miss, account for more TAG access
	 	if ((hit_where1 != (HitWhere::where_t)m_mem_component) || 
	  	(hit_where2 != (HitWhere::where_t)m_mem_component)) {
      	getMemoryManager()->incrElapsedTime(m_mem_component, 
					CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);
      	getMemoryManager()->incrElapsedTime(m_mem_component, 
				CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);
	 	}
	 	//Finally account for PIC operation 1.75 * DATA
	 	//TODO: Make this 1.75
   	getMemoryManager()->incrElapsedTime(m_mem_component, 
				CachePerfModel::ACCESS_CACHE_DATA, ShmemPerfModel::_USER_THREAD);
   	getMemoryManager()->incrElapsedTime(m_mem_component, 
				CachePerfModel::ACCESS_CACHE_DATA, ShmemPerfModel::_USER_THREAD);

		picUpdateCounters(pic_opcode, ca_address1, hit_where1, 
										ca_address2, hit_where2);
		LOG_PRINT("\nL2%d+%lx..+%lx, %s,%s", (int)pic_opcode, 
				ca_address1, ca_address2, HitWhereString(hit_where1), 
				HitWhereString(hit_where2));
   	return ((hit_where1 > hit_where2) ? hit_where1 : hit_where2);
	}
	else {
		//send to next level, take corrective actions if any
    getMemoryManager()->incrElapsedTime(m_mem_component, 
			CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);
    getMemoryManager()->incrElapsedTime(m_mem_component, 
		CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);

		picCorrectiveMeasures(pic_opcode, ca_address1, ca_address2);

    HitWhere::where_t hit_where = HitWhere::MISS;
		if(!m_next_cache_cntlr) {
				//Check if we need to insert a dummy key address
				if(pic_opcode == PIC_SEARCH) {
					if(getHome(ca_address1) != getHome(ca_address2)) {
						IntPtr dummy_key_address = 0;
						//data home > key home
						int diff = getHome(ca_address1) - getHome(ca_address2);
						diff = (diff < 0) ? (diff * -1) : diff;
						dummy_key_address = ca_address2 + (diff * 64); 
						//assert(getHome(ca_address1) > getHome(ca_address2));
						//dummy_key_address = ca_address2 + (getHome(ca_address1) - getHome(ca_address2)) * 64; 
						assert(dummy_key_address != 0);
						ca_address2 = dummy_key_address;
					}
				}
    		acquireStackLock(ca_address2);
				initiatePicDirectoryAccess(pic_opcode, ca_address1, ca_address2, 
																		t_issue);
      	releaseStackLock(ca_address2);
				//wait for it
      	waitForNetworkThread();
      	wakeUpNetworkThread();
				//Doing this under lock
				assert(m_last_remote_hit_where != HitWhere::UNKNOWN);
				hit_where = m_last_remote_hit_where;
				m_last_remote_hit_where = HitWhere::UNKNOWN;
      	releaseStackLock(ca_address2);
				return hit_where;
		}
		else { 
			assert(m_next_cache_cntlr);
    	hit_where = 
									m_next_cache_cntlr->processPicReqFromPrevCache(this, 
									pic_opcode, ca_address1, ca_address2, true, true,
									t_issue);
			return hit_where;
		}
	}
}

HitWhere::where_t
CacheCntlr::processShmemReqFromPrevCache(CacheCntlr* requester, Core::mem_op_t mem_op_type, IntPtr address, bool modeled, bool count, Prefetch::prefetch_type_t isPrefetch, SubsecondTime t_issue, bool have_write_lock, IntPtr other_pic_address, IntPtr other_pic_address2)
{
   #ifdef PRIVATE_L2_OPTIMIZATION
   bool have_write_lock_internal = have_write_lock;
   if (! have_write_lock && m_shared_cores > 1)
   {
      acquireStackLock(address, true);
      have_write_lock_internal = true;
   }
   #else
   bool have_write_lock_internal = true;
   #endif

   bool cache_hit = operationPermissibleinCache(address, mem_op_type), sibling_hit = false, prefetch_hit = false;
   bool first_hit = cache_hit;
   HitWhere::where_t hit_where = HitWhere::MISS;
   SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);

   if (!cache_hit && m_perfect)
   {
      cache_hit = true;
      hit_where = HitWhere::where_t(m_mem_component);
      if (cache_block_info)
         cache_block_info->setCState(CacheState::MODIFIED);
      else
         cache_block_info = insertCacheBlock(address, mem_op_type == Core::READ ? CacheState::SHARED : CacheState::MODIFIED, NULL, m_core_id, ShmemPerfModel::_USER_THREAD);
   }

   if (count)
   {
      ScopedLock sl(getLock());
      if (isPrefetch == Prefetch::NONE)
         getCache()->updateCounters(cache_hit);
      updateCounters(mem_op_type, address, cache_hit, getCacheState(address), isPrefetch);
   }

   if (cache_hit)
   {
      if (isPrefetch == Prefetch::NONE && cache_block_info->hasOption(CacheBlockInfo::PREFETCH))
      {
         // This line was fetched by the prefetcher and has proven useful
         stats.hits_prefetch++;
         prefetch_hit = true;
         cache_block_info->clearOption(CacheBlockInfo::PREFETCH);
      }
      if (cache_block_info->hasOption(CacheBlockInfo::WARMUP) && Sim()->getInstrumentationMode() != InstMode::CACHE_ONLY)
      {
         stats.hits_warmup++;
         cache_block_info->clearOption(CacheBlockInfo::WARMUP);
      }

      // Increment Shared Mem Perf model cycle counts
      /****** TODO: for the last-level cache, this is also done by the network thread when the message comes in.
                    we probably shouldn't do this twice */
      /* TODO: if we end up getting the data from a sibling cache, the access time might be only that
         of the previous-level cache, not our (longer) access time */
      if (modeled)
      {
         ScopedLock sl(getLock());
         // This is a hit, but maybe the prefetcher filled it at a future time stamp. If so, delay.
         SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
         if (m_master->mshr.count(address)
            && (m_master->mshr[address].t_issue < t_now && m_master->mshr[address].t_complete > t_now))
         {
            SubsecondTime latency = m_master->mshr[address].t_complete - t_now;
            stats.mshr_latency += latency;
            getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
         }
         else
         {
            getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS, ShmemPerfModel::_USER_THREAD);
         }
      }

      if (mem_op_type != Core::READ) // write that hits
      {
         /* Invalidate/flush in previous levels */
         SubsecondTime latency = SubsecondTime::Zero();
         for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); it != m_master->m_prev_cache_cntlrs.end(); it++)
         {
            if (*it != requester)
            {
               std::pair<SubsecondTime, bool> res = (*it)->updateCacheBlock(address, CacheState::INVALID, Transition::COHERENCY, NULL, ShmemPerfModel::_USER_THREAD);
               latency = getMax<SubsecondTime>(latency, res.first);
               sibling_hit |= res.second;
            }
         }
         MYLOG("add latency %s, sibling_hit(%u)", itostr(latency).c_str(), sibling_hit);
         getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
         atomic_add_subsecondtime(stats.snoop_latency, latency);
         #ifdef ENABLE_TRACK_SHARING_PREVCACHES
         assert(! cache_block_info->hasCachedLoc());
         #endif
      }
      else if (cache_block_info->getCState() == CacheState::MODIFIED) // reading MODIFIED data
      {
         MYLOG("reading MODIFIED data");
         /* Writeback in previous levels */
         SubsecondTime latency = SubsecondTime::Zero();
         for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); it != m_master->m_prev_cache_cntlrs.end(); it++) {
            if (*it != requester) {
               std::pair<SubsecondTime, bool> res = (*it)->updateCacheBlock(address, CacheState::SHARED, Transition::COHERENCY, NULL, ShmemPerfModel::_USER_THREAD);
               latency = getMax<SubsecondTime>(latency, res.first);
               sibling_hit |= res.second;
            }
         }
         MYLOG("add latency %s, sibling_hit(%u)", itostr(latency).c_str(), sibling_hit);
         getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
         atomic_add_subsecondtime(stats.snoop_latency, latency);
      }
      else if (cache_block_info->getCState() == CacheState::EXCLUSIVE) // reading EXCLUSIVE data
      {
         MYLOG("reading EXCLUSIVE data");
         // will have shared state
         SubsecondTime latency = SubsecondTime::Zero();
         for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); it != m_master->m_prev_cache_cntlrs.end(); it++) {
            if (*it != requester) {
               std::pair<SubsecondTime, bool> res = (*it)->updateCacheBlock(address, CacheState::SHARED, Transition::COHERENCY, NULL, ShmemPerfModel::_USER_THREAD);
               latency = getMax<SubsecondTime>(latency, res.first);
               sibling_hit |= res.second;
            }
         }

         MYLOG("add latency %s, sibling_hit(%u)", itostr(latency).c_str(), sibling_hit);
         getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
         atomic_add_subsecondtime(stats.snoop_latency, latency);
      }

      if (m_last_remote_hit_where != HitWhere::UNKNOWN)
      {
         // handleMsgFromDramDirectory just provided us with the data. Its source was left in m_last_remote_hit_where
         hit_where = m_last_remote_hit_where;
         m_last_remote_hit_where = HitWhere::UNKNOWN;
      }
      else
      	hit_where = HitWhere::where_t(m_mem_component + (sibling_hit ? HitWhere::SIBLING : 0));

   }
   else // !cache_hit: either data is not here, or operation on data is not permitted
   {
      // Increment shared mem perf model cycle counts
      if (modeled)
         getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);

      if (cache_block_info && (cache_block_info->getCState() == CacheState::SHARED))
      {
         // Data is present, but still no cache_hit => this is a write on a SHARED block. Do Upgrade
         SubsecondTime latency = SubsecondTime::Zero();
         for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); it != m_master->m_prev_cache_cntlrs.end(); it++)
            if (*it != requester)
               latency = getMax<SubsecondTime>(latency, (*it)->updateCacheBlock(address, CacheState::INVALID, Transition::UPGRADE, NULL, ShmemPerfModel::_USER_THREAD).first);
         getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
         atomic_add_subsecondtime(stats.snoop_latency, latency);
         #ifdef ENABLE_TRACK_SHARING_PREVCACHES
         assert(! cache_block_info->hasCachedLoc());
         #endif
      }

      if (m_next_cache_cntlr)
      {
         if (cache_block_info)
            invalidateCacheBlock(address);

         // let the next cache level handle it.
         hit_where = m_next_cache_cntlr->processShmemReqFromPrevCache(this, mem_op_type, address, modeled, count, isPrefetch == Prefetch::NONE ? Prefetch::NONE : Prefetch::OTHER, t_issue, have_write_lock_internal, other_pic_address, other_pic_address2);
         if (hit_where != HitWhere::MISS)
         {
            cache_hit = true;
            /* get the data for ourselves */
            SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
            copyDataFromNextLevel(mem_op_type, address, modeled, t_now, other_pic_address, other_pic_address2);
            if (isPrefetch != Prefetch::NONE)
               getCacheBlockInfo(address)->setOption(CacheBlockInfo::PREFETCH);
         }
      }
      else // last-level cache
      {
         if (cache_block_info && cache_block_info->getCState() == CacheState::EXCLUSIVE)
         {
            // Data is present, but still no cache_hit => this is a write on a SHARED block. Do Upgrade
            SubsecondTime latency = SubsecondTime::Zero();
            for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); it != m_master->m_prev_cache_cntlrs.end(); it++)
               if (*it != requester)
                  latency = getMax<SubsecondTime>(latency, (*it)->updateCacheBlock(address, CacheState::INVALID, Transition::UPGRADE, NULL, ShmemPerfModel::_USER_THREAD).first);
            getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
            atomic_add_subsecondtime(stats.snoop_latency, latency);
            #ifdef ENABLE_TRACK_SHARING_PREVCACHES
            assert(! cache_block_info->hasCachedLoc());
            #endif

            cache_hit = true;
            hit_where = HitWhere::where_t(m_mem_component);
            MYLOG("Silent upgrade from E -> M for address %lx", address);
            cache_block_info->setCState(CacheState::MODIFIED);
         }
         else if (m_master->m_dram_cntlr)
         {
            // Direct DRAM access
            cache_hit = true;
            if (cache_block_info)
            {
               // We already have the line: it must have been SHARED and this is a write (else there wouldn't have been a miss)
               // Upgrade silently
               cache_block_info->setCState(CacheState::MODIFIED);
               hit_where = HitWhere::where_t(m_mem_component);
            }
            else
            {
               Byte data_buf[getCacheBlockSize()];
               SubsecondTime latency;
							 bool dram_accessed =  false;
							 if(!m_pic_avoid_dram ||
									(!((other_pic_address != 0) &&  (mem_op_type == Core::WRITE))))
							 {
               // Do the DRAM access and increment local time
							 dram_accessed =  true;
               m_shmem_perf->reset(getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD));
               		boost::tie<HitWhere::where_t, SubsecondTime>(hit_where, 
									latency) = accessDRAM(Core::READ, address, 
										isPrefetch != Prefetch::NONE, data_buf);
               		getMemoryManager()->incrElapsedTime(latency, 
										ShmemPerfModel::_USER_THREAD);
							 }
							 if(m_pic_avoid_dram && ((other_pic_address != 0) && 
													(mem_op_type == Core::WRITE)))
							 	hit_where = HitWhere::DRAM;
               // Insert the line. Be sure to use SHARED/MODIFIED as appropriate (upgrades are free anyway), we don't want to have to write back clean lines
               insertCacheBlock(address, mem_op_type == Core::READ ? CacheState::SHARED : CacheState::MODIFIED, data_buf, m_core_id, ShmemPerfModel::_USER_THREAD, 									other_pic_address, other_pic_address2);
               if (isPrefetch != Prefetch::NONE)
                  getCacheBlockInfo(address)->setOption(CacheBlockInfo::PREFETCH);
							 if(dram_accessed)
               updateUncoreStatistics(hit_where, getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD));
            }
         }
         else
         {
            initiateDirectoryAccess(mem_op_type, address, 
						isPrefetch != Prefetch::NONE, t_issue, other_pic_address, other_pic_address2);
						if(m_pic_on) {
							assert(pendingPic.find(address) == pendingPic.end());
							if(other_pic_address) {
								std::pair<IntPtr, IntPtr> pic_pair(other_pic_address, other_pic_address2);
								pendingPic[address] = pic_pair;
							}
						}
         }
      }
   }

   if (cache_hit)
   {
      MYLOG("Yay, hit!!");
      Byte data_buf[getCacheBlockSize()];
      retrieveCacheBlock(address, data_buf, ShmemPerfModel::_USER_THREAD, first_hit && count);
      /* Store completion time so we can detect overlapping accesses */
      if (modeled && !first_hit)
      {
         ScopedLock sl(getLock());
         m_master->mshr[address] = make_mshr(t_issue, getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD));
         cleanupMshr();
      }
   }

   if (modeled && m_master->m_prefetcher)
   {
      trainPrefetcher(address, cache_hit, prefetch_hit, t_issue);
   }

   #ifdef PRIVATE_L2_OPTIMIZATION
   if (have_write_lock_internal && !have_write_lock)
   {
      releaseStackLock(address, true);
   }
   #else
   #endif

   MYLOG("\nreturning %s", HitWhereString(hit_where));
   return hit_where;
}

void
CacheCntlr::notifyPrevLevelInsert(core_id_t core_id, MemComponent::component_t mem_component, IntPtr address)
{
   #ifdef ENABLE_TRACK_SHARING_PREVCACHES
   SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);
   assert(cache_block_info);
   PrevCacheIndex idx = m_master->m_prev_cache_cntlrs.find(core_id, mem_component);
   cache_block_info->setCachedLoc(idx);
   #endif
}

void
CacheCntlr::notifyPrevLevelEvict(core_id_t core_id, MemComponent::component_t mem_component, IntPtr address)
{
MYLOG("@%lx", address);
   if (m_master->m_evicting_buf && address == m_master->m_evicting_address) {
MYLOG("here being evicted");
   } else {
      #ifdef ENABLE_TRACK_SHARING_PREVCACHES
      SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);
MYLOG("here in state %c", CStateString(getCacheState(address)));
      assert(cache_block_info);
      PrevCacheIndex idx = m_master->m_prev_cache_cntlrs.find(core_id, mem_component);
      cache_block_info->clearCachedLoc(idx);
      #endif
   }
}

void
CacheCntlr::updateUsageBits(IntPtr address, CacheBlockInfo::BitsUsedType used)
{
   bool new_bits;
   {
      ScopedLock sl(getLock());
      SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);
      new_bits = cache_block_info->updateUsage(used);
   }
   if (new_bits && m_next_cache_cntlr && !m_perfect)
   {
      m_next_cache_cntlr->updateUsageBits(address, used);
   }
}

void
CacheCntlr::walkUsageBits()
{
   if (!m_next_cache_cntlr && Sim()->getConfig()->hasCacheEfficiencyCallbacks())
   {
      for(UInt32 set_index = 0; set_index < m_master->m_cache->getNumSets(); ++set_index)
      {
         for(UInt32 way = 0; way < m_master->m_cache->getAssociativity(); ++way)
         {
            CacheBlockInfo *block_info = m_master->m_cache->peekBlock(set_index, way);
            if (block_info->isValid() && !block_info->hasOption(CacheBlockInfo::WARMUP))
            {
               Sim()->getConfig()->getCacheEfficiencyCallbacks().call_notify_evict(true, block_info->getOwner(), 0, block_info->getUsage(), getCacheBlockSize() >> CacheBlockInfo::BitsUsedOffset);
            }
         }
      }
   }
}

boost::tuple<HitWhere::where_t, SubsecondTime>
CacheCntlr::accessDRAM(Core::mem_op_t mem_op_type, IntPtr address, bool isPrefetch, Byte* data_buf)
{
   ScopedLock sl(getLock()); // DRAM is shared and owned by m_master

   SubsecondTime t_issue = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
   SubsecondTime dram_latency;
   HitWhere::where_t hit_where;

   switch (mem_op_type)
   {
      case Core::READ:
         boost::tie(dram_latency, hit_where) = m_master->m_dram_cntlr->getDataFromDram(address, m_core_id_master, data_buf, t_issue, m_shmem_perf);
         break;

      case Core::READ_EX:
      case Core::WRITE:
         boost::tie(dram_latency, hit_where) = m_master->m_dram_cntlr->putDataToDram(address, m_core_id_master, data_buf, t_issue);
         break;

      default:
         LOG_PRINT_ERROR("Unsupported Mem Op Type(%u)", mem_op_type);
   }

   return boost::tuple<HitWhere::where_t, SubsecondTime>(hit_where, dram_latency);
}

void
CacheCntlr::initiateDirectoryAccess(Core::mem_op_t mem_op_type, IntPtr address, bool isPrefetch, SubsecondTime t_issue, IntPtr other_pic_address
, IntPtr other_pic_address2)
{
   bool exclusive = false;
   switch (mem_op_type) {
      case Core::READ:
         exclusive = false;
         break;
      case Core::READ_EX:
      case Core::WRITE:
         exclusive = true;
         break;
      default:
         LOG_PRINT_ERROR("Unsupported Mem Op Type(%u)", mem_op_type);
   }
   bool first = false;
   {
      ScopedLock sl(getLock());
      CacheDirectoryWaiter* request = 
				new CacheDirectoryWaiter(exclusive, isPrefetch, this, t_issue);
      m_master->m_directory_waiters.enqueue(address, request);
      if (m_master->m_directory_waiters.size(address) == 1)
         first = true;
   }
   if (first) {
      m_shmem_perf->reset(getShmemPerfModel()->getElapsedTime
				(ShmemPerfModel::_USER_THREAD));
      /* We're the first one to request this address, send the message to the directory now */
      if (exclusive) {
         SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);
         if (cache_block_info && 
						(cache_block_info->getCState() == CacheState::SHARED)) {
            processUpgradeReqToDirectory(address, m_shmem_perf, 
						other_pic_address, other_pic_address2);
         }
         else {
            processExReqToDirectory(address, other_pic_address, other_pic_address2);
         }
      }
      else {
         processShReqToDirectory(address, other_pic_address, other_pic_address2);
      }
   }
   else
   {
    // Someone else is busy with this cache line, they'll do everything for us
    MYLOG("%u previous waiters", m_master->m_directory_waiters.size(address));
   }
}

void
CacheCntlr::processExReqToDirectory(IntPtr address, IntPtr other_pic_address, IntPtr other_pic_address2)
{
   // We need to send a request to the Dram Directory Cache
   MYLOG("EX REQ>%d @ %lx", getHome(address) ,address);

   CacheState::cstate_t cstate = getCacheState(address);

   LOG_ASSERT_ERROR (cstate != CacheState::SHARED, "ExReq for a Cacheblock in S, should be a UpgradeReq");
   assert((cstate == CacheState::INVALID));

   getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::EX_REQ,
         MemComponent::LAST_LEVEL_CACHE, MemComponent::TAG_DIR,
         m_core_id_master /* requester */,
         getHome(address) /* receiver */,
         address,
         NULL, 0,
         HitWhere::UNKNOWN, m_shmem_perf, ShmemPerfModel::_USER_THREAD,
				 other_pic_address, other_pic_address2);
}

void
CacheCntlr::processUpgradeReqToDirectory(IntPtr address, ShmemPerf *perf, 
IntPtr other_pic_address, IntPtr other_pic_address2)
{
   // We need to send a request to the Dram Directory Cache
   MYLOG("UPGR REQ @ %lx", address);

   CacheState::cstate_t cstate = getCacheState(address);
   assert(cstate == CacheState::SHARED);
   setCacheState(address, CacheState::SHARED_UPGRADING);

   getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::UPGRADE_REQ,
         MemComponent::LAST_LEVEL_CACHE, MemComponent::TAG_DIR,
         m_core_id_master /* requester */,
         getHome(address) /* receiver */,
         address,
         NULL, 0,
         HitWhere::UNKNOWN, perf, ShmemPerfModel::_USER_THREAD,
				 other_pic_address, other_pic_address2);
}

void
CacheCntlr::processShReqToDirectory(IntPtr address, IntPtr other_pic_address, IntPtr other_pic_address2)
{
MYLOG("SH REQ @ %lx", address);
   getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::SH_REQ,
         MemComponent::LAST_LEVEL_CACHE, MemComponent::TAG_DIR,
         m_core_id_master /* requester */,
         getHome(address) /* receiver */,
         address,
         NULL, 0,
         HitWhere::UNKNOWN, m_shmem_perf, ShmemPerfModel::_USER_THREAD,
				 other_pic_address, other_pic_address2);
}






/*****************************************************************************
 * internal operations called by cache on itself
 *****************************************************************************/

bool
CacheCntlr::operationPermissibleinCache(
      IntPtr address, Core::mem_op_t mem_op_type, CacheBlockInfo **cache_block_info)
{
   CacheBlockInfo *block_info = getCacheBlockInfo(address);
   if (cache_block_info != NULL)
      *cache_block_info = block_info;

   bool cache_hit = false;
   CacheState::cstate_t cstate = getCacheState(block_info);

   switch (mem_op_type)
   {
      case Core::READ:
         cache_hit = CacheState(cstate).readable();
         break;

      case Core::READ_EX:
      case Core::WRITE:
         cache_hit = CacheState(cstate).writable();
         break;

      default:
         LOG_PRINT_ERROR("Unsupported mem_op_type: %u", mem_op_type);
         break;
   }

   MYLOG("address %lx state %c: permissible %d", address, CStateString(cstate), cache_hit);
   return cache_hit;
}


void
CacheCntlr::accessCache(
      Core::mem_op_t mem_op_type, IntPtr ca_address, UInt32 offset,
      Byte* data_buf, UInt32 data_length, bool update_replacement)
{
   switch (mem_op_type)
   {
      case Core::READ:
      case Core::READ_EX:
         m_master->m_cache->accessSingleLine(ca_address + offset, Cache::LOAD, data_buf, data_length,
                                             getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD), update_replacement);
         break;

      case Core::WRITE:
         m_master->m_cache->accessSingleLine(ca_address + offset, Cache::STORE, data_buf, data_length,
                                             getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD), update_replacement);
         // Write-through cache - Write the next level cache also
         if (m_cache_writethrough) {
            LOG_ASSERT_ERROR(m_next_cache_cntlr, "Writethrough enabled on last-level cache !?");
MYLOG("writethrough start");
            m_next_cache_cntlr->writeCacheBlock(ca_address, offset, data_buf, data_length, ShmemPerfModel::_USER_THREAD);
MYLOG("writethrough done");
         }
         break;

      default:
         LOG_PRINT_ERROR("Unsupported Mem Op Type: %u", mem_op_type);
         break;
   }
}




/*****************************************************************************
 * localized cache block operations
 *****************************************************************************/

SharedCacheBlockInfo*
CacheCntlr::getCacheBlockInfo(IntPtr address)
{
   return (SharedCacheBlockInfo*) m_master->m_cache->peekSingleLine(address);
}

CacheState::cstate_t
CacheCntlr::getCacheState(IntPtr address)
{
   SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);
   return getCacheState(cache_block_info);
}

CacheState::cstate_t
CacheCntlr::getCacheState(CacheBlockInfo *cache_block_info)
{
   return (cache_block_info == NULL) ? CacheState::INVALID : cache_block_info->getCState();
}

SharedCacheBlockInfo*
CacheCntlr::setCacheState(IntPtr address, CacheState::cstate_t cstate)
{
   SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);
   cache_block_info->setCState(cstate);
   return cache_block_info;
}

void
CacheCntlr::invalidateCacheBlock(IntPtr address)
{
   __attribute__((unused)) CacheState::cstate_t old_cstate = getCacheState(address);
   assert(old_cstate != CacheState::MODIFIED);
   assert(old_cstate != CacheState::INVALID);

   m_master->m_cache->invalidateSingleLine(address);

   if (m_next_cache_cntlr)
      m_next_cache_cntlr->notifyPrevLevelEvict(m_core_id_master, m_mem_component, address);

   MYLOG("%lx %c > %c", address, CStateString(old_cstate), CStateString(getCacheState(address)));
}

void
CacheCntlr::retrieveCacheBlock(IntPtr address, Byte* data_buf, ShmemPerfModel::Thread_t thread_num, bool update_replacement)
{
   __attribute__((unused)) SharedCacheBlockInfo* cache_block_info = (SharedCacheBlockInfo*) m_master->m_cache->accessSingleLine(
      address, Cache::LOAD, data_buf, getCacheBlockSize(), getShmemPerfModel()->getElapsedTime(thread_num), update_replacement);
   LOG_ASSERT_ERROR(cache_block_info != NULL, "Expected block to be there but it wasn't");
}

/*****************************************************************************
 * CAP: Update latency metrics
 *****************************************************************************/
void CacheCntlr::updateCAPLatency()
{  
   SubsecondTime latency = SubsecondTime::Zero();

   latency += m_data_access_time.getLatency();  
   latency += m_tags_access_time.getLatency();  

   getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
}

/*****************************************************************************
 * CAP: Program the swizzle switch for the current state with corresponding 
 *      next_state vectors
 *****************************************************************************/
void CacheCntlr::updateSwizzleSwitch(UInt32 steNum_BytePos, Byte* nextStateInfo, UInt32 data_length)
{
   //UInt32 nextStateVecLength = SWIZZLE_SWITCH_Y; 
   memcpy((m_swizzleSwitch+steNum_BytePos), nextStateInfo, data_length);
}

/*****************************************************************************
 * CAP: Program the reporting STE information per sub array
 *****************************************************************************/
void CacheCntlr::updateReportingSteInfo(UInt32 bytePos, Byte *data_buf, UInt32 data_length)
{
   memcpy((m_reportingSteInfo + bytePos), data_buf, data_length);
}

/*****************************************************************************
 * CAP: Program the initial start state mask into the currStateMask at the 
 * beginning of the sim
 *****************************************************************************/
void CacheCntlr::updateStartStateMask(UInt32 bytePos, Byte *data_buf, UInt32 data_length)
{
   memcpy((m_startSTEMask + bytePos), data_buf, data_length);
   memcpy((m_currStateMask + bytePos), data_buf, data_length);  // also update the curr state mask the first time
}

/*****************************************************************************
 * CAP: Display the content of the swizzle switch
 *****************************************************************************/

void CacheCntlr::showSwizzleSwitch()  
{
   Byte singleByte;
   printf("CAP:::: SWIZZLE SWITCH\n");
   for (int i=0; i<SWIZZLE_SWITCH_X; i++)  {
      for (int j=0; j<SWIZZLE_SWITCH_Y; j++)  {
         singleByte = *(m_swizzleSwitch + (i*SWIZZLE_SWITCH_Y + j));
         printf("%d ", (UInt32)(singleByte));
      }
      printf("\n");
   }
}


/*****************************************************************************
 * CAP: Perform a swizle switch lookup using curr_state and output next_state vector
 *
 * Description:
 * Reads the incoming bit stream comprising of current state STE bits.
 * If any of the STE bits is set, the corresponding next state STE bits are read out 
 * There is additional logic to account for multiple STEs in current state
 * 
 *****************************************************************************/
void CacheCntlr::retrieveNextStateInfo(Byte* inDataBuf, Byte* outDataBuf)
{
   UInt32 currStateVecLength = SWIZZLE_SWITCH_X; 
   UInt32 nextStateVecLength = SWIZZLE_SWITCH_Y; 

   UInt32 i=0, j=0, k=0, l=0;
   Byte* tempOutDataBuf = new Byte[nextStateVecLength];
   Byte trueOutByte, tempOutByte;
   bool activeStateFound = 0;


   for (i=0; i<nextStateVecLength; i++) {
      Byte dataByte;  // temp var to store each byte from input currState vector
      memcpy(&dataByte, inDataBuf+i, 1);

      while (j<8) {
         if ((Byte)(dataByte>>(7-j)) & 0x1) {   // check if last bit is 1
            int byteToReadFrom = (i*8+j)*nextStateVecLength;  // skip every 64B chunk

            memcpy(tempOutDataBuf, m_swizzleSwitch+byteToReadFrom, nextStateVecLength);  // swizzle switch look-up

            if(DEBUG_ENABLED)  {
               printf("Found next state vector at STE:%d at byte position: %d\n", (i*8+j), byteToReadFrom);
               for (l=0; l<nextStateVecLength; l++)  
                  printf("%d\t", (Byte)(*(tempOutDataBuf+l)));
               printf ("\n");
            }

            if (activeStateFound) { // this means more than one active curr_state bit in input
               while (k<nextStateVecLength)  {
                  memcpy(&trueOutByte, outDataBuf+k, 1);
                  memcpy(&tempOutByte, tempOutDataBuf+k, 1);
                  trueOutByte = trueOutByte | tempOutByte;
                  memcpy(outDataBuf+k, &trueOutByte, 1);
                  ++k;
               }
               k = 0;
            }

            // copy into outDataBuf just during first hit to ensure that if another hit is found, outDataBuf does not have junk values
            if (!activeStateFound)  
               memcpy(outDataBuf, m_swizzleSwitch+byteToReadFrom, nextStateVecLength); 

            activeStateFound = 1; // set flag to denote next_state found
         }
         ++j;
      }
      j = 0;
   }

   delete tempOutDataBuf;
   LOG_ASSERT_ERROR(activeStateFound == 1, "No next_states were found for currently active state!");
}

/*****************************************************************************
 * CAP: parent function which gets the input character, 
 * accesses the cache subarrays and concatenates the curr_state vectors from 
 * each subarray, performs a lookup in the swizzle switch and estimates 
 * next_state vectors and writes back into the curr_state mask register
 *****************************************************************************/
void CacheCntlr::processPatternMatch(UInt32 inputChar)
{
   UInt32 subarrayIndexBits = 0, k=0, i=0;
   UInt32 address;
   IntPtr addr;
   Byte* temp_data_buf = new Byte[m_cache_block_size];
   Byte* data_buf = new Byte[NUM_SUBARRAYS*m_cache_block_size];
   Byte* out_data_buf = new Byte[NUM_SUBARRAYS*m_cache_block_size];
   Byte tempA_data_buf, tempB_data_buf;
   UInt32 nextStateVecLength = SWIZZLE_SWITCH_Y;
   UInt32 m_log_blocksize = floorLog2(m_cache_block_size);

   while(subarrayIndexBits<NUM_SUBARRAYS){
      // encode the single input character into N addresses for lookup in N subarrays
      address = (subarrayIndexBits<<(m_logASCIISetIndex+m_log_blocksize)) | (inputChar<<m_log_blocksize);
      UInt32 offset = address & (m_log_blocksize-1);
      UInt32 addrAligned = address & (~(m_log_blocksize-1));
      addr = (IntPtr)addrAligned;

      if (DEBUG_ENABLED)  printf("processPatternMatch: Value read from Cache at full addr 0x%x addr_aligned: 0x%x offset: 0x%x for input (int)%d (char)%c \n", address, addrAligned, offset, (char)(inputChar), (UInt32)(inputChar));

      printf ("Reading input char: %c\n", (char)(inputChar));

      accessCache(Core::READ, addr, 0, temp_data_buf, m_cache_block_size, 1);

      //Byte_pos = 0 means it's the first character. If its first character, then load the curr state mask
      //if(!byte_pos)   
      //   memcpy(m_currStateMask,temp_data_buf, SWIZZLE_SWITCH_Y);

      if(DEBUG_ENABLED)  {
         // print the cache line
         for (i=0; i<m_cache_block_size; i++)  
            printf("%d\t", (Byte)(*(temp_data_buf+i)));
         printf ("\n");
      }

      memcpy((data_buf+(subarrayIndexBits*m_cache_block_size)), temp_data_buf, m_cache_block_size);
      ++subarrayIndexBits;
      updateCAPLatency();
   }

   if(DEBUG_ENABLED)  {
      // print the reporting STE info
      printf ("Reporting STE info:\n");
      for (i=0; i<SWIZZLE_SWITCH_Y; i++)  
         printf("%d\t", (Byte)(*(m_reportingSteInfo+i)));
      printf("\n");
   }

   if(DEBUG_ENABLED)  {
      // print the starting state mask
      printf ("Start state mask info:\n");
      for (i=0; i<SWIZZLE_SWITCH_Y; i++)  
         printf("%d\t", (Byte)(*(m_startSTEMask+i)));
      printf("\n");

      // print the current state info before masking
      printf ("Current state info before masking: \n");
      for (i=0; i<m_cache_block_size; i++)  
         printf("%d\t", (Byte)(*(data_buf+i)));
      printf("\n");
   }
   
   bool activeCurrStFound = 0;

   // Mask curr state vectors read from cache subarrays with the current state Mask 
   while (k<nextStateVecLength)  {
      memcpy(&tempA_data_buf, data_buf+k, 1);
      memcpy(&tempB_data_buf, m_currStateMask+k, 1);
      tempA_data_buf = tempA_data_buf & tempB_data_buf;

      if (tempA_data_buf) 
         activeCurrStFound = 1;

      memcpy(data_buf+k, &tempA_data_buf, 1);
      ++k;
   }

   if(DEBUG_ENABLED)  {
      // print the current state info after masking
      printf ("Current state info after masking: \n");
      for (i=0; i<m_cache_block_size; i++)  
         printf("%d\t", (Byte)(*(data_buf+i)));
      printf("\n");
   }

   if (!activeCurrStFound) {
      printf("No active state found for current input. Invalid transition... Resetting...\n");

      // update the start state mask
      memcpy(m_currStateMask, m_startSTEMask, (NUM_SUBARRAYS*m_cache_block_size));
   }
   else { // an active current state has been found

      // Look up in the swizzle switch to get the next_state vector
      retrieveNextStateInfo(data_buf, out_data_buf);
     
      if(DEBUG_ENABLED)  {
         // print the next state info
         printf ("Overall next state info: \n");
         for (i=0; i<m_cache_block_size; i++)  
            printf("%d\t", (Byte)(*(out_data_buf+i)));
         printf("\n");
      }

      // Update the mask register with next state bits 
      k=0;
      while (k<nextStateVecLength)  {
         memcpy(m_currStateMask+k, out_data_buf+k, 1);
         ++k;
      }

      // Reporting STE detection
      k = 0;
      while (k<nextStateVecLength)  {
         memcpy(&tempA_data_buf, m_currStateMask+k, 1);
         memcpy(&tempB_data_buf, m_reportingSteInfo+k, 1);
         tempA_data_buf = tempA_data_buf & tempB_data_buf;
         if (tempA_data_buf) {
            printf("Yaay! FSM Match found! \n");
            m_numFSMmatches++;
             // update the start state mask
            memcpy(m_currStateMask, m_startSTEMask, (NUM_SUBARRAYS*m_cache_block_size));
           //exit(0);
         }
         ++k;
      }
      
   }
}

// CAP:
HitWhere::where_t
CacheCntlr::processCAPSOpFromCore(
			CacheCntlr::cap_ops_t cap_op,
            IntPtr addr,
            Byte* data_buf, 
            UInt32 data_length)  {
   static int m_gl_count = 0;
   static int first_print = 0;
   if(DEBUG_ENABLED)  printf("processCAPSOpFromCore: CAP OPCODE :%d, addr (hex): 0x%x data: (int)%d (char)%c\n", (int)cap_op, (UInt32)(addr), (Byte)(*data_buf), (char)(*data_buf));

   if (cap_op == CacheCntlr::CAP_SS)  {  // call the updateSwizzleSwitch function. I know this is redundant.
      UInt32 steNum = (UInt32)(addr) / SWIZZLE_SWITCH_Y;
      UInt32 bytePos = (UInt32)(addr) % SWIZZLE_SWITCH_Y;

      if(DEBUG_ENABLED)  printf("processCAPSOpFromCore: CAP OPCODE :%d, STE (hex): 0x%x, STE Num: %d, byte position: %d\n", (int)cap_op, (UInt32)(addr), steNum, bytePos);

      UInt32 steNum_BytePos = (UInt32)(addr);
      updateSwizzleSwitch(steNum_BytePos, data_buf, data_length);

      // Add time
      getMemoryManager()->incrElapsedTime(m_ss_program_time.getLatency(), ShmemPerfModel::_USER_THREAD);
      m_gl_count++;
      
      // printf("gl_count=%d\n",m_gl_count);
      if(DEBUG_ENABLED) {
         if (m_gl_count == SWIZZLE_SWITCH_X*SWIZZLE_SWITCH_Y)  // print SS contents after all programming
            showSwizzleSwitch();
      }
   }

   else if (cap_op == CacheCntlr::CAP_REP_STE)  { // reporting STE programming logic
      UInt32 bytePos = (UInt32)(addr);
      updateReportingSteInfo(bytePos, data_buf, data_length);
   }
   else if (cap_op == CacheCntlr::CAP_ST_MASK)  { // start mask programming
      UInt32 bytePos = (UInt32)(addr);
      updateStartStateMask(bytePos, data_buf, data_length);
   }
   else {
      if (!first_print) {
         printf ("\n\n Reading input pattern...\n\n");
         first_print++;
      }
      UInt32 bytePos = (UInt32)(addr);
      processPatternMatch((Byte)(*data_buf));
   }

   return HitWhere::L1_OWN;  // TODO: HACK: always return hit in L1
   
}


/*****************************************************************************
 * cache block operations that update the previous level(s)
 *****************************************************************************/

SharedCacheBlockInfo*
CacheCntlr::insertCacheBlock(IntPtr address, CacheState::cstate_t cstate, Byte* data_buf, core_id_t requester, ShmemPerfModel::Thread_t thread_num, 
IntPtr other_pic_address, IntPtr other_pic_address2)
{
MYLOG("insertCacheBlock l%d @ %lx as %c (now %c)", m_mem_component, address, CStateString(cstate), CStateString(getCacheState(address)));
   bool eviction;
   IntPtr evict_address;
   SharedCacheBlockInfo evict_block_info;
   Byte evict_buf[getCacheBlockSize()];

   LOG_ASSERT_ERROR(getCacheState(address) == CacheState::INVALID, "we already have this line, can't add it again");

   m_master->m_cache->insertSingleLine(address, data_buf,
         &eviction, &evict_address, &evict_block_info, evict_buf,
         getShmemPerfModel()->getElapsedTime(thread_num), this, other_pic_address, other_pic_address2);
   SharedCacheBlockInfo* cache_block_info = setCacheState(address, cstate);

   if (Sim()->getInstrumentationMode() == InstMode::CACHE_ONLY)
      cache_block_info->setOption(CacheBlockInfo::WARMUP);

   if (Sim()->getConfig()->hasCacheEfficiencyCallbacks())
      cache_block_info->setOwner(Sim()->getConfig()->getCacheEfficiencyCallbacks().call_get_owner(requester, address));

   if (m_next_cache_cntlr && !m_perfect)
      m_next_cache_cntlr->notifyPrevLevelInsert(m_core_id_master, m_mem_component, address);
MYLOG("insertCacheBlock l%d local done", m_mem_component);


   if (eviction)
   {
MYLOG("evicting @%lx", evict_address);

      if (
         !m_next_cache_cntlr // Track at LLC
         && !evict_block_info.hasOption(CacheBlockInfo::WARMUP) // Ignore blocks allocated during warmup (we don't track usage then)
         && Sim()->getConfig()->hasCacheEfficiencyCallbacks()
      )
      {
         Sim()->getConfig()->getCacheEfficiencyCallbacks().call_notify_evict(false, evict_block_info.getOwner(), cache_block_info->getOwner(), evict_block_info.getUsage(), getCacheBlockSize() >> CacheBlockInfo::BitsUsedOffset);
      }

      CacheState::cstate_t old_state = evict_block_info.getCState();
      MYLOG("evicting @%lx (state %c)", evict_address, CStateString(old_state));
      {
         ScopedLock sl(getLock());
         transition(
            evict_address,
            Transition::EVICT,
            old_state,
            CacheState::INVALID
         );

         ++stats.evict[old_state];
         // Line was prefetched, but is evicted without ever being used
         if (evict_block_info.hasOption(CacheBlockInfo::PREFETCH))
            ++stats.evict_prefetch;
         if (evict_block_info.hasOption(CacheBlockInfo::WARMUP))
            ++stats.evict_warmup;
      }

      /* TODO: this part looks a lot like updateCacheBlock's dirty case, but with the eviction buffer
         instead of an address, and with a message to the directory at the end. Merge? */

      LOG_PRINT("Eviction: addr(0x%x)", evict_address);
      if (! m_master->m_prev_cache_cntlrs.empty()) {
         ScopedLock sl(getLock());
         /* propagate the update to the previous levels. they will write modified data back to our evict buffer when needed */
         m_master->m_evicting_address = evict_address;
         m_master->m_evicting_buf = evict_buf;

         SubsecondTime latency = SubsecondTime::Zero();
         for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); it != m_master->m_prev_cache_cntlrs.end(); it++)
            latency = getMax<SubsecondTime>(latency, (*it)->updateCacheBlock(evict_address, CacheState::INVALID, Transition::BACK_INVAL, NULL, thread_num).first);
         getMemoryManager()->incrElapsedTime(latency, thread_num);
         atomic_add_subsecondtime(stats.snoop_latency, latency);

         m_master->m_evicting_address = 0;
         m_master->m_evicting_buf = NULL;
      }

      /* now properly get rid of the evicted line */

      if (m_perfect)
      {
         // Nothing to do in this case
      }
      else if (!m_coherent)
      {
         // Don't notify the next level, it may have already evicted the line itself and won't like our notifyPrevLevelEvict
         // Make sure the line wasn't modified though (unless we're writethrough), else data would have been lost
         if (!m_cache_writethrough)
            LOG_ASSERT_ERROR(evict_block_info.getCState() != CacheState::MODIFIED, "Non-coherent cache is throwing away dirty data");
      }
      else if (m_next_cache_cntlr)
      {
         if (m_cache_writethrough) {
            /* If we're a write-through cache the new data is in the next level already */
         } else {
            /* Send dirty block to next level cache. Probably we have an evict/victim buffer to do that when we're idle, so ignore timing */
            if (evict_block_info.getCState() == CacheState::MODIFIED) {
               m_next_cache_cntlr->writeCacheBlock(evict_address, 0, evict_buf, getCacheBlockSize(), thread_num, true);
				 			//STAT_FIX: Count eviction only if dirty
							{
               	ScopedLock sl(getLock());
								++stats.dirty_evicts;
							}
						}
         }
         m_next_cache_cntlr->notifyPrevLevelEvict(m_core_id_master, m_mem_component, evict_address);
      }
      else if (m_master->m_dram_cntlr)
      {
         if (evict_block_info.getCState() == CacheState::MODIFIED)
         {
				 		//STAT_FIX: Count eviction only if dirty
						{
             	ScopedLock sl(getLock());
							++stats.dirty_evicts;
						}
            SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);

            if (m_master->m_dram_outstanding_writebacks)
            {
               ScopedLock sl(getLock());
               // Delay if all evict buffers are full
               SubsecondTime t_issue = m_master->m_dram_outstanding_writebacks->getStartTime(t_now);
               getMemoryManager()->incrElapsedTime(t_issue - t_now, ShmemPerfModel::_USER_THREAD);
            }

            // Access DRAM
            SubsecondTime dram_latency;
            HitWhere::where_t hit_where;
            boost::tie<HitWhere::where_t, SubsecondTime>(hit_where, dram_latency) = accessDRAM(Core::WRITE, evict_address, false, evict_buf);

            // Occupy evict buffer
            if (m_master->m_dram_outstanding_writebacks)
            {
               ScopedLock sl(getLock());
               m_master->m_dram_outstanding_writebacks->getCompletionTime(t_now, dram_latency);
            }
         }
      }
      else
      {
         /* Send dirty block to directory */
         UInt32 home_node_id = getHome(evict_address);
         if (evict_block_info.getCState() == CacheState::MODIFIED)
         {
				 		//STAT_FIX: Count eviction only if dirty
						{
              ScopedLock sl(getLock());
							++stats.dirty_evicts;
						}
            // Send back the data also
MYLOG("evict FLUSH %lx", evict_address);
            getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::FLUSH_REP,
                  MemComponent::LAST_LEVEL_CACHE, MemComponent::TAG_DIR,
                  m_core_id /* requester */,
                  home_node_id /* receiver */,
                  evict_address,
                  evict_buf, getCacheBlockSize(),
                  HitWhere::UNKNOWN, NULL, thread_num);
         }
         else
         {
MYLOG("evict INV %lx", evict_address);
            LOG_ASSERT_ERROR(evict_block_info.getCState() == CacheState::SHARED || evict_block_info.getCState() == CacheState::EXCLUSIVE,
                  "evict_address(0x%x), evict_state(%u)",
                  evict_address, evict_block_info.getCState());
            getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::INV_REP,
                  MemComponent::LAST_LEVEL_CACHE, MemComponent::TAG_DIR,
                  m_core_id /* requester */,
                  home_node_id /* receiver */,
                  evict_address,
                  NULL, 0,
                  HitWhere::UNKNOWN, NULL, thread_num);
         }
      }

      LOG_ASSERT_ERROR(getCacheState(evict_address) == CacheState::INVALID, "Evicted address did not become invalid, now in state %s", CStateString(getCacheState(evict_address)));
      MYLOG("insertCacheBlock l%d evict done", m_mem_component);
   }

   MYLOG("insertCacheBlock l%d end", m_mem_component);
   return cache_block_info;
}

std::pair<SubsecondTime, bool>
CacheCntlr::updateCacheBlock(IntPtr address, CacheState::cstate_t new_cstate, Transition::reason_t reason, Byte* out_buf, ShmemPerfModel::Thread_t thread_num)
{
   MYLOG("updateCacheBlock");
   LOG_ASSERT_ERROR(new_cstate < CacheState::NUM_CSTATE_STATES, "Invalid new cstate %u", new_cstate);

   /* first, propagate the update to the previous levels. they will write modified data back to us when needed */
   // TODO: performance fix: should only query those we know have the line (in cache_block_info->m_cached_locs)
   /* TODO: increment time (access tags) on previous-level caches, either all (for a snooping protocol that's not keeping track
              of cache_block_info->m_cached_locs) or selective (snoop filter / directory) */
   SubsecondTime latency = SubsecondTime::Zero();
   bool sibling_hit = false;

   if (! m_master->m_prev_cache_cntlrs.empty())
   {
      for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); it != m_master->m_prev_cache_cntlrs.end(); it++) {
         std::pair<SubsecondTime, bool> res = (*it)->updateCacheBlock(
            address, new_cstate, reason == Transition::EVICT ? Transition::BACK_INVAL : reason, NULL, thread_num);
         // writeback_time is for the complete stack, so only model it at the last level, ignore latencies returned by previous ones
         //latency = getMax<SubsecondTime>(latency, res.first);
         sibling_hit |= res.second;
      }
   }

   SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);
   __attribute__((unused)) CacheState::cstate_t old_cstate = cache_block_info ? cache_block_info->getCState() : CacheState::INVALID;

   bool buf_written = false, is_writeback = false;

   if (!cache_block_info)
   {
      /* We don't have the block, nothing to do */
   }
   else if (new_cstate == cache_block_info->getCState() && out_buf)
   {
      // We already have the right state, nothing to do except writing our data
      // in the out_buf if it is passed
         // someone (presumably the directory interfacing code) is waiting to consume the data
      retrieveCacheBlock(address, out_buf, thread_num, false);
      buf_written = true;
      is_writeback = true;
      sibling_hit = true;
   }
   else
   {
      {
         ScopedLock sl(getLock());
         transition(
            address,
            reason,
            getCacheState(address),
            new_cstate
         );
         if (reason == Transition::COHERENCY)
         {
            if (new_cstate == CacheState::SHARED)
               ++stats.coherency_downgrades;
            else if (cache_block_info->getCState() == CacheState::MODIFIED)
               ++stats.coherency_writebacks;
            else
               ++stats.coherency_invalidates;
            if (cache_block_info->hasOption(CacheBlockInfo::PREFETCH) && new_cstate == CacheState::INVALID)
               ++stats.invalidate_prefetch;
            if (cache_block_info->hasOption(CacheBlockInfo::WARMUP) && new_cstate == CacheState::INVALID)
               ++stats.invalidate_warmup;
         }
         if (reason == Transition::UPGRADE)
         {
            ++stats.coherency_upgrades;
         }
         else if (reason == Transition::BACK_INVAL)
         {
            ++stats.backinval[cache_block_info->getCState()];
         }
      }

      if (cache_block_info->getCState() == CacheState::MODIFIED) {
         /* data is modified, write it back */

         if (m_cache_writethrough) {
            /* next level already has the data */

         } else if (m_next_cache_cntlr) {
            /* write straight into the next level cache */
				 		//STAT_FIX: Count back_invalidate only if dirty
						{
             	ScopedLock sl(getLock());
							++stats.dirty_backinval;
						}
            Byte data_buf[getCacheBlockSize()];
            retrieveCacheBlock(address, data_buf, thread_num, false);
            m_next_cache_cntlr->writeCacheBlock(address, 0, data_buf, getCacheBlockSize(), thread_num, true);
            is_writeback = true;
            sibling_hit = true;

         } else if (out_buf) {
            /* someone (presumably the directory interfacing code) is waiting to consume the data */
				 		//STAT_FIX: Count back_invalidate only if dirty
						{
             	ScopedLock sl(getLock());
							++stats.dirty_backinval;
						}
            retrieveCacheBlock(address, out_buf, thread_num, false);
            buf_written = true;
            is_writeback = true;
            sibling_hit = true;

         } else {
            /* no-one will take my data !? */
            LOG_ASSERT_ERROR( cache_block_info->getCState() != CacheState::MODIFIED, "MODIFIED data is about to get lost!");

         }
         cache_block_info->setCState(CacheState::SHARED);
      }

      if (new_cstate == CacheState::INVALID)
      {
         if (out_buf)
         {
            retrieveCacheBlock(address, out_buf, thread_num, false);
            buf_written = true;
            is_writeback = true;
            sibling_hit = true;
         }
         if (m_coherent)
            invalidateCacheBlock(address);
      }
      else if (new_cstate == CacheState::SHARED)
      {
         if (out_buf)
         {
            retrieveCacheBlock(address, out_buf, thread_num, false);
            buf_written = true;
            is_writeback = true;
            sibling_hit = true;
         }

         cache_block_info->setCState(new_cstate);
      }
      else if (new_cstate == CacheState::MODIFIED)
      {
         cache_block_info->setCState(new_cstate);
      }
      else
      {
         LOG_ASSERT_ERROR(false, "Cannot change block status to %c", CStateString(new_cstate));
      }
   }

   MYLOG("@%lx  %c > %c (req: %c)", address, CStateString(old_cstate),
                                     CStateString(cache_block_info ? cache_block_info->getCState() : CacheState::INVALID),
                                     CStateString(new_cstate));

   LOG_ASSERT_ERROR((getCacheState(address) == CacheState::INVALID) || (getCacheState(address) == new_cstate) || !m_coherent,
         "state didn't change as we wanted: %c instead of %c", CStateString(getCacheState(address)), CStateString(new_cstate));

   CacheState::cstate_t current_cstate;
   current_cstate = (cache_block_info) ? cache_block_info->getCState(): CacheState::INVALID;
   LOG_ASSERT_ERROR((current_cstate == CacheState::INVALID) || (current_cstate == new_cstate) || !m_coherent,
         "state didn't change as we wanted: %c instead of %c", CStateString(current_cstate), CStateString(new_cstate));
   if (out_buf && !buf_written)
   {
      MYLOG("cache_block_info: %c", cache_block_info ? 'y' : 'n');
      MYLOG("@%lx  %c > %c (req: %c)", address, CStateString(old_cstate),
                                           CStateString(cache_block_info ? cache_block_info->getCState() : CacheState::INVALID),
                                           CStateString(new_cstate));
   }
   LOG_ASSERT_ERROR(out_buf ? buf_written : true, "out_buf passed in but never written to");
   /* Assume tag access caused by snooping is already accounted for in lower level cache access time,
      so only when we accessed data should we return any latency */
   if (is_writeback)
      latency += m_writeback_time.getLatency();
   return std::pair<SubsecondTime, bool>(latency, sibling_hit);
}

void
CacheCntlr::writeCacheBlock(IntPtr address, UInt32 offset, Byte* data_buf, UInt32 data_length, ShmemPerfModel::Thread_t thread_num, bool count)
{
MYLOG(" ");

   // TODO: should we update access counter?
	 //STAT_FIX: Count writeback, this is called for modified data only
	 if(count)
		{
     	//ScopedLock sl(getLock());
			++stats.writebacks;
		}
   if (m_master->m_evicting_buf && (address == m_master->m_evicting_address)) {
      MYLOG("writing to evict buffer %lx", address);
assert(offset==0);
assert(data_length==getCacheBlockSize());
      if (data_buf)
         memcpy(m_master->m_evicting_buf + offset, data_buf, data_length);
   } else {
      __attribute__((unused)) SharedCacheBlockInfo* cache_block_info = (SharedCacheBlockInfo*) m_master->m_cache->accessSingleLine(
         address + offset, Cache::STORE, data_buf, data_length, getShmemPerfModel()->getElapsedTime(thread_num), false);
      LOG_ASSERT_ERROR(cache_block_info, "writethrough expected a hit at next-level cache but got miss");
      LOG_ASSERT_ERROR(cache_block_info->getCState() == CacheState::MODIFIED, "Got writeback for non-MODIFIED line");
   }

   if (m_cache_writethrough) {
      acquireStackLock(true);
      m_next_cache_cntlr->writeCacheBlock(address, offset, data_buf, data_length, thread_num);
      releaseStackLock(true);
   }
}

bool
CacheCntlr::isInLowerLevelCache(CacheBlockInfo *block_info)
{
   IntPtr address = m_master->m_cache->tagToAddress(block_info->getTag());
   for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); it != m_master->m_prev_cache_cntlrs.end(); it++)
   {
      // All caches are inclusive, so there is no need to propagate further
      SharedCacheBlockInfo* block_info = (*it)->getCacheBlockInfo(address);
      if (block_info != NULL)
         return true;
   }
   return false;
}

void
CacheCntlr::incrementQBSLookupCost()
{
   SubsecondTime latency = m_writeback_time.getLatency();
   getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
   atomic_add_subsecondtime(stats.qbs_query_latency, latency);
}


/*****************************************************************************
 * handle messages from directory (in network thread)
 *****************************************************************************/

void
CacheCntlr::handleMsgFromDramDirectory(
      core_id_t sender, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg)
{
   PrL1PrL2DramDirectoryMSI::ShmemMsg::msg_t shmem_msg_type = shmem_msg->getMsgType();
   IntPtr address = shmem_msg->getAddress();

#ifdef PIC_ENABLE_OPERATIONS
//if(shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_COPY_REP)
//printf("\nVPIC_COPY_REP<%u @ %lx", sender, address);
//if(shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_CMP_REP)
//printf("\nVPIC_CMP_REP<%u @ %lx", sender, address);
#endif

   core_id_t requester = INVALID_CORE_ID;
   if ((shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::EX_REP) 
		|| (shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::SH_REP)
    || (shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::UPGRADE_REP) 
		//#ifdef PIC_ENABLE_OPERATIONS
		|| (shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_COPY_REP)
		|| (shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_CMP_REP)
		|| (shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_SEARCH_REP)
		//#endif
		)
   {
      ScopedLock sl(getLock()); // Keep lock when handling m_directory_waiters
      CacheDirectoryWaiter* request = 
				m_master->m_directory_waiters.front(address);
      requester = request->cache_cntlr->m_core_id;
   }

   acquireStackLock(address);
MYLOG("begin");

   switch (shmem_msg_type)
   {
		//#ifdef PIC_ENABLE_OPERATIONS
      case PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_COPY_REP:
      case PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_CMP_REP:
      case PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_SEARCH_REP:
			break;
		//#endif
      case PrL1PrL2DramDirectoryMSI::ShmemMsg::EX_REP:
MYLOG("EX REP<%u @ %lx", sender, address);
         processExRepFromDramDirectory(sender, requester, shmem_msg);
         break;
      case PrL1PrL2DramDirectoryMSI::ShmemMsg::SH_REP:
MYLOG("SH REP<%u @ %lx", sender, address);
         processShRepFromDramDirectory(sender, requester, shmem_msg);
         break;
      case PrL1PrL2DramDirectoryMSI::ShmemMsg::UPGRADE_REP:
MYLOG("UPGR REP<%u @ %lx", sender, address);
         processUpgradeRepFromDramDirectory(sender, requester, shmem_msg);
         break;
      case PrL1PrL2DramDirectoryMSI::ShmemMsg::INV_REQ:
MYLOG("INV REQ<%u @ %lx", sender, address);
         processInvReqFromDramDirectory(sender, shmem_msg);
         break;
      case PrL1PrL2DramDirectoryMSI::ShmemMsg::FLUSH_REQ:
MYLOG("FLUSH REQ<%u @ %lx", sender, address);
         processFlushReqFromDramDirectory(sender, shmem_msg);
         break;
      case PrL1PrL2DramDirectoryMSI::ShmemMsg::WB_REQ:
MYLOG("WB REQ<%u @ %lx", sender, address);
         processWbReqFromDramDirectory(sender, shmem_msg);
         break;
      default:
         LOG_PRINT_ERROR("Unrecognized msg type: %u", shmem_msg_type);
         break;
   }

   if ((shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::EX_REP) 
		|| (shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::SH_REP)
    || (shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::UPGRADE_REP)
		//#ifdef PIC_ENABLE_OPERATIONS
		|| (shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_COPY_REP)
		|| (shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_CMP_REP)
		|| (shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_SEARCH_REP)
		//#endif
		)
   {
      getLock().acquire(); // Keep lock when handling m_directory_waiters
      while(! m_master->m_directory_waiters.empty(address)) {
         CacheDirectoryWaiter* request = 
						m_master->m_directory_waiters.front(address);
         getLock().release();

				 //If this was pic operation done in last level.. you dnt have
				 //cache block
				 //#ifdef PIC_ENABLE_OPERATIONS
					if(
						(shmem_msg_type != 
							PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_COPY_REP)
							&& 
						(shmem_msg_type != 
							PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_CMP_REP)
							&& 
						(shmem_msg_type != 
							PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_SEARCH_REP)
						)
				 //#endif
				 {
         	if (request->exclusive && 
								(getCacheState(address) == CacheState::SHARED)) {
         		MYLOG("have SHARED, upgrading to MODIFIED for #%u", 
						request->cache_cntlr->m_core_id);
          	// We (the master cache) are sending the upgrade request in 
						// place of request->cache_cntlr,
            // so use their ShmemPerf* rather than ours
            processUpgradeReqToDirectory(address, 
											request->cache_cntlr->m_shmem_perf);
            releaseStackLock(address);
            return;
         	}
         	if (request->isPrefetch)
            getCacheBlockInfo(address)->setOption(CacheBlockInfo::PREFETCH);
				 }

         // Set the Counters in the Shmem Perf model accordingly
         // Set the counter value in the USER thread to that in the SIM thread
         SubsecondTime t_core = 
					request->cache_cntlr->getShmemPerfModel()->getElapsedTime
					(ShmemPerfModel::_USER_THREAD),
          t_here = getShmemPerfModel()->getElapsedTime
					(ShmemPerfModel::_SIM_THREAD);

         if (t_here > t_core) {
					MYLOG("adjusting time in #%u from %lu to %lu", 
					request->cache_cntlr->m_core_id, t_core.getNS(), t_here.getNS());
          /* Unless the requesting core is already ahead of us (it may very well be if this cache's master thread's cpu
          is falling behind), update its time */
          // TODO: update master thread time in initiateDirectoryAccess ?
          request->cache_cntlr->getShmemPerfModel()->setElapsedTime
										(ShmemPerfModel::_USER_THREAD, t_here);
         }

					MYLOG("wakeup user #%u", request->cache_cntlr->m_core_id);
         	request->cache_cntlr->updateUncoreStatistics
													(shmem_msg->getWhere(), t_here);
         //releaseStackLock(address);
         // Pass stack lock through to user thread
         wakeUpUserThread(request->cache_cntlr->m_user_thread_sem);
         waitForUserThread(request->cache_cntlr->m_network_thread_sem);
         acquireStackLock(address);
				 //#ifdef PIC_ENABLE_OPERATIONS
					if(
(shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_COPY_REP) ||
(shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_CMP_REP) ||
(shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_SEARCH_REP)
							) {
assert(shmem_msg->m_other_address);
ScopedLock sl(request->cache_cntlr->getLock());
request->cache_cntlr->m_master->mshr_pic[shmem_msg->m_other_address] = 
make_mshr(request->t_issue, 
getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD));
					}
				 //#endif
					if(
(shmem_msg_type != PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_COPY_REP) ||
(shmem_msg_type != PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_CMP_REP) ||
(shmem_msg_type != PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_SEARCH_REP)
							)
         {
            ScopedLock sl(request->cache_cntlr->getLock());
            request->cache_cntlr->m_master->mshr[address] = 
						make_mshr(request->t_issue, 
						getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD));
            cleanupMshr();
         }
         getLock().acquire();
         MYLOG("about to dequeue request (%p) for address %lx", 
					m_master->m_directory_waiters.front(address), address );
         m_master->m_directory_waiters.dequeue(address);
         delete request;

				 //#ifdef PIC_ENABLE_OPERATIONS
					if(
							(shmem_msg_type ==
								PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_COPY_REP) 
							||
							(shmem_msg_type ==
								PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_CMP_REP) 
							||
							(shmem_msg_type ==
								PrL1PrL2DramDirectoryMSI::ShmemMsg::VPIC_SEARCH_REP) 
						)
      			assert(m_master->m_directory_waiters.empty(address));
				 //#endif
      }
      getLock().release();
			MYLOG("woke up all");
   }
   releaseStackLock(address);
	 MYLOG("done");
}

void
CacheCntlr::processExRepFromDramDirectory(core_id_t sender, core_id_t requester, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg)
{
   // Forward data from message to LLC, don't incur LLC data access time (writeback will be done asynchronously)
   //getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS);
MYLOG("processExRepFromDramDirectory l%d", m_mem_component);

   IntPtr address = shmem_msg->getAddress();
   Byte* data_buf = shmem_msg->getDataBuf();

		IntPtr other_pic_address = 0;
		IntPtr other_pic_address2 = 0;
		if(m_pic_on) {
			if(pendingPic.find(address) != pendingPic.end()) {
				other_pic_address		= pendingPic[address].first;
				other_pic_address2 	= pendingPic[address].second;
				pendingPic.erase(address);
			}
			if(pic_other_load_address) {
				assert(other_pic_address == 0);
				other_pic_address 	= pic_other_load_address;
				other_pic_address2	= pic_other_load2_address;
			}
		}

   insertCacheBlock(address, CacheState::EXCLUSIVE, data_buf, requester, ShmemPerfModel::_SIM_THREAD, other_pic_address, other_pic_address2);
MYLOG("processExRepFromDramDirectory l%d end", m_mem_component);
}

void
CacheCntlr::processShRepFromDramDirectory(core_id_t sender, core_id_t requester, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg)
{
   // Forward data from message to LLC, don't incur LLC data access time (writeback will be done asynchronously)
   //getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS);
MYLOG("processShRepFromDramDirectory l%d", m_mem_component);

   IntPtr address = shmem_msg->getAddress();
   Byte* data_buf = shmem_msg->getDataBuf();
		IntPtr other_pic_address = 0;
		IntPtr other_pic_address2 = 0;
		if(m_pic_on) {
			if(pendingPic.find(address) != pendingPic.end()) {
				other_pic_address		= pendingPic[address].first;
				other_pic_address2 	= pendingPic[address].second;
				pendingPic.erase(address);
			}
			if(pic_other_load_address) {
				assert(other_pic_address == 0);
				other_pic_address = pic_other_load_address;
				other_pic_address2	= pic_other_load2_address;
			}
		}

   // Insert Cache Block in L2 Cache
   insertCacheBlock(address, CacheState::SHARED, data_buf, requester, ShmemPerfModel::_SIM_THREAD, other_pic_address, other_pic_address2);
}

void
CacheCntlr::processUpgradeRepFromDramDirectory(core_id_t sender, core_id_t requester, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg)
{
MYLOG("processShRepFromDramDirectory l%d", m_mem_component);
   // We now have the only copy. Change to a writeable state.
   IntPtr address = shmem_msg->getAddress();
   CacheState::cstate_t cstate = getCacheState(address);

   if (cstate == CacheState::INVALID)
   {
      // I lost my copy because a concurrent UPGRADE REQ had INVed it, because the state
      // was Modified  when this request was processed, the data should be in the message
      // because it was FLUSHed (see dram_directory_cntlr.cc, MODIFIED case of the upgrade req)
      Byte* data_buf = shmem_msg->getDataBuf();
      LOG_ASSERT_ERROR(data_buf, "Trying to upgrade a block that is now INV and no data in the shmem_msg");

      updateCacheBlock(address, CacheState::MODIFIED, Transition::UPGRADE, data_buf, ShmemPerfModel::_SIM_THREAD);
   }
   else if  (cstate == CacheState::SHARED_UPGRADING)
   {
      // Last-Level Cache received a upgrade REP, but multiple private lower-level caches might
      // still have a shared copy. Should invalidate all except the ones from the core that initiated
      // the upgrade request (sender).

      updateCacheBlock(address, CacheState::MODIFIED, Transition::UPGRADE, NULL, ShmemPerfModel::_SIM_THREAD);
   }
   else
   {
      LOG_PRINT_ERROR("Trying to upgrade a block that is not SHARED_UPGRADING but %c (%lx)",CStateString(cstate), address);
   }
}

void
CacheCntlr::processInvReqFromDramDirectory(core_id_t sender, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();
MYLOG("processInvReqFromDramDirectory l%d", m_mem_component);

   CacheState::cstate_t cstate = getCacheState(address);
   if (cstate != CacheState::INVALID)
   {
      if (cstate != CacheState::SHARED)
      {
        MYLOG("invalidating something else than SHARED: %c ", CStateString(cstate));
      }
      //assert(cstate == CacheState::SHARED);
      shmem_msg->getPerf()->updateTime(getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD));

      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_SIM_THREAD);

      updateCacheBlock(address, CacheState::INVALID, Transition::COHERENCY, NULL, ShmemPerfModel::_SIM_THREAD);

      shmem_msg->getPerf()->updateTime(getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD), ShmemPerf::REMOTE_CACHE_INV);

      getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::INV_REP,
            MemComponent::LAST_LEVEL_CACHE, MemComponent::TAG_DIR,
            shmem_msg->getRequester() /* requester */,
            sender /* receiver */,
            address,
            NULL, 0,
            HitWhere::UNKNOWN, shmem_msg->getPerf(), ShmemPerfModel::_SIM_THREAD);
   }
   else
   {
      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_SIM_THREAD);
MYLOG("invalid @ %lx, hoping eviction message is underway", address);
   }
}

void
CacheCntlr::processFlushReqFromDramDirectory(core_id_t sender, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();
MYLOG("processFlushReqFromDramDirectory l%d", m_mem_component);

   CacheState::cstate_t cstate = getCacheState(address);
   if (cstate != CacheState::INVALID)
   {
      //assert(cstate == CacheState::MODIFIED);
      shmem_msg->getPerf()->updateTime(getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD));

      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS, ShmemPerfModel::_SIM_THREAD);

      // Flush the line
      Byte data_buf[getCacheBlockSize()];
      updateCacheBlock(address, CacheState::INVALID, Transition::COHERENCY, data_buf, ShmemPerfModel::_SIM_THREAD);

      shmem_msg->getPerf()->updateTime(getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD), ShmemPerf::REMOTE_CACHE_WB);

      getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::FLUSH_REP,
            MemComponent::LAST_LEVEL_CACHE, MemComponent::TAG_DIR,
            shmem_msg->getRequester() /* requester */,
            sender /* receiver */,
            address,
            data_buf, getCacheBlockSize(),
            HitWhere::UNKNOWN, shmem_msg->getPerf(), ShmemPerfModel::_SIM_THREAD);
   }
   else
   {
      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_SIM_THREAD);
MYLOG("invalid @ %lx, hoping eviction message is underway", address);
   }
}

void
CacheCntlr::processWbReqFromDramDirectory(core_id_t sender, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();
MYLOG("processWbReqFromDramDirectory l%d", m_mem_component);

   CacheState::cstate_t cstate = getCacheState(address);
   if (cstate != CacheState::INVALID)
   {
      //assert(cstate == CacheState::MODIFIED);
      shmem_msg->getPerf()->updateTime(getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD));

      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS, ShmemPerfModel::_SIM_THREAD);

      // Write-Back the line
      Byte data_buf[getCacheBlockSize()];
      if (cstate != CacheState::SHARED_UPGRADING)
      {
         updateCacheBlock(address, CacheState::SHARED, Transition::COHERENCY, data_buf, ShmemPerfModel::_SIM_THREAD);
      }

      shmem_msg->getPerf()->updateTime(getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD), ShmemPerf::REMOTE_CACHE_WB);

      getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::WB_REP,
            MemComponent::LAST_LEVEL_CACHE, MemComponent::TAG_DIR,
            shmem_msg->getRequester() /* requester */,
            sender /* receiver */,
            address,
            data_buf, getCacheBlockSize(),
            HitWhere::UNKNOWN, shmem_msg->getPerf(), ShmemPerfModel::_SIM_THREAD);
   }
   else
   {
      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_SIM_THREAD);
MYLOG("invalid @ %lx, hoping eviction message is underway", address);
   }
}



/*****************************************************************************
 * statistics functions
 *****************************************************************************/

void
CacheCntlr::updateCounters(Core::mem_op_t mem_op_type, IntPtr address, bool cache_hit, CacheState::cstate_t state, Prefetch::prefetch_type_t isPrefetch)
{
   /* If another miss to this cache line is still in progress:
      operationPermissibleinCache() will think it's a hit (so cache_hit == true) since the processing
      of the previous miss was done instantaneously. But mshr[address] contains its completion time */
   SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
   bool overlapping = m_master->mshr.count(address) && m_master->mshr[address].t_issue < t_now && m_master->mshr[address].t_complete > t_now;

   // ATD doesn't track state, so when reporting hit/miss to it we shouldn't either (i.e. write hit to shared line becomes hit, not miss)
   bool cache_data_hit = (state != CacheState::INVALID);
   m_master->accessATDs(mem_op_type, cache_data_hit, address, m_core_id - m_core_id_master);

   if (mem_op_type == Core::WRITE)
   {
      if (isPrefetch != Prefetch::NONE)
         stats.stores_prefetch++;
      if (isPrefetch != Prefetch::OWN)
      {
         stats.stores++;
         stats.stores_state[state]++;
         if (! cache_hit || overlapping) {
            stats.store_misses++;
            stats.store_misses_state[state]++;
            if (overlapping) stats.store_overlapping_misses++;
         }
      }
   }
   else
   {
      if (isPrefetch != Prefetch::NONE)
         stats.loads_prefetch++;
      if (isPrefetch != Prefetch::OWN)
      {
         stats.loads++;
         stats.loads_state[state]++;
         if (! cache_hit) {
            stats.load_misses++;
            stats.load_misses_state[state]++;
            if (overlapping) stats.load_overlapping_misses++;
         }
      }
   }

   cleanupMshr();

   #ifdef ENABLE_TRANSITIONS
   transition(
      address,
      mem_op_type == Core::WRITE ? Transition::CORE_WR : (mem_op_type == Core::READ_EX ? Transition::CORE_RDEX : Transition::CORE_RD),
      state,
      mem_op_type == Core::READ && state != CacheState::MODIFIED ? CacheState::SHARED : CacheState::MODIFIED
   );
   #endif
}

void
CacheCntlr::cleanupMshr()
{
   /* Keep only last 8 MSHR entries */
   while(m_master->mshr.size() > 8) {
      IntPtr address_min = 0;
      SubsecondTime time_min = SubsecondTime::MaxTime();
      for(Mshr::iterator it = m_master->mshr.begin(); it != m_master->mshr.end(); ++it) {
         if (it->second.t_complete < time_min) {
            address_min = it->first;
            time_min = it->second.t_complete;
         }
      }
      m_master->mshr.erase(address_min);
   }
}
void CacheCntlr::addLatencyMshrPic(IntPtr ca_address) {
	ScopedLock sl(getLock());
	SubsecondTime latency = SubsecondTime::Zero();
	// Serialize PIC ops
  SubsecondTime t_now = 
		getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
  if (m_master->mshr_pic.count(ca_address)
  	&& (m_master->mshr_pic[ca_address].t_issue < 
			t_now && m_master->mshr_pic[ca_address].t_complete > t_now)) {
  	latency = 
			m_master->mshr_pic[ca_address].t_complete - t_now;
    getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
		cleanupMshrPic(t_now);
	}
}
void
CacheCntlr::cleanupMshrPic(SubsecondTime t_start)
{
   bool deleted = true;
   while(deleted) {
      IntPtr address_min = 0;
			deleted = false;
      for(Mshr::iterator it = m_master->mshr_pic.begin(); 
				 it != m_master->mshr_pic.end(); ++it) {
         if (it->second.t_complete < t_start) {
            address_min = it->first;
						deleted = true;
						break;
         }
      }
      m_master->mshr_pic.erase(address_min);
   }
}

void
CacheCntlr::transition(IntPtr address, Transition::reason_t reason, CacheState::cstate_t old_state, CacheState::cstate_t new_state)
{
#ifdef ENABLE_TRANSITIONS
   stats.transitions[old_state][new_state]++;
   if (old_state == CacheState::INVALID) {
      if (stats.seen.count(address) == 0)
         old_state = CacheState::INVALID_COLD;
      else if (stats.seen[address] == Transition::EVICT || stats.seen[address] == Transition::BACK_INVAL)
         old_state = CacheState::INVALID_EVICT;
      else if (stats.seen[address] == Transition::COHERENCY)
         old_state = CacheState::INVALID_COHERENCY;
   }
   stats.transition_reasons[reason][old_state][new_state]++;
   stats.seen[address] = reason;
#endif
}

void
CacheCntlr::updateUncoreStatistics(HitWhere::where_t hit_where, SubsecondTime now)
{
   // To be propagated through next-level refill
   m_last_remote_hit_where = hit_where;

   // Update ShmemPerf
   if (m_shmem_perf->getInitialTime() > SubsecondTime::Zero())
   {
      // We can't really be sure that there are not outstanding transations that still pass a pointer
      // around to our ShmemPerf structure. By settings its last time to MaxTime, we prevent anyone
      // from updating statistics while we're reading them.
      m_shmem_perf->disable();

      m_shmem_perf_global->add(m_shmem_perf);
      m_shmem_perf_totaltime += now - m_shmem_perf->getInitialTime();
      m_shmem_perf_numrequests ++;

      m_shmem_perf->reset(SubsecondTime::Zero());
   }
}


/*****************************************************************************
 * utility functions
 *****************************************************************************/

/* Lock design:
   - we want to allow concurrent access for operations that only touch the first-level cache
   - we want concurrent access to different addresses as much as possible

   Master last-level cache contains one shared/exclusive-lock per set (according to the first-level cache's set size)
   - First-level cache transactions acquire the lock pertaining to the set they'll use in shared mode.
     Multiple first-level caches can do this simultaneously.
     Since only a single thread accesses each L1, there should be no extra per-cache lock needed
     (The above is not strictly true, but Core takes care of this since MemoryManager only has one cycle count anyway).
     On a miss, a lock upgrade is needed.
   - Other levels, or the first level on miss, acquire the lock in exclusive mode which locks out both L1-only and L2+ transactions.
   #ifdef PRIVATE_L2_OPTIMIZATION
   - (On Nehalem, the L2 is private so it is only the L3 (the first level with m_sharing_cores > 1) that takes the exclusive lock).
   #endif

   Additionally, for per-cache objects that are not private to a cache set, each cache controller has its own (normal) lock,
   use getLock() for this. This is required for statistics updates, the directory waiters queue, etc.
*/

void
CacheCntlr::acquireLock(UInt64 address)
{
MYLOG("cache lock acquire %u # %u @ %lx", m_mem_component, m_core_id, address);
   assert(isFirstLevel());
   // Lock this L1 cache for the set containing <address>.
   lastLevelCache()->m_master->getSetLock(address)->acquire_shared(m_core_id);
}

void
CacheCntlr::releaseLock(UInt64 address)
{
MYLOG("cache lock release %u # %u @ %lx", m_mem_component, m_core_id, address);
   assert(isFirstLevel());
   lastLevelCache()->m_master->getSetLock(address)->release_shared(m_core_id);
}

void
CacheCntlr::acquireStackLock(UInt64 address, bool this_is_locked)
{
MYLOG("stack lock acquire %u # %u @ %lx", m_mem_component, m_core_id, address);
   // Lock the complete stack for the set containing <address>
   if (this_is_locked)
      // If two threads decide to upgrade at the same time, we could deadlock.
      // Upgrade therefore internally releases the cache lock!
      lastLevelCache()->m_master->getSetLock(address)->upgrade(m_core_id);
   else
      lastLevelCache()->m_master->getSetLock(address)->acquire_exclusive();
}

void
CacheCntlr::releaseStackLock(UInt64 address, bool this_is_locked)
{
MYLOG("stack lock release %u # %u @ %lx", m_mem_component, m_core_id, address);
   if (this_is_locked)
      lastLevelCache()->m_master->getSetLock(address)->downgrade(m_core_id);
   else
      lastLevelCache()->m_master->getSetLock(address)->release_exclusive();
}


CacheCntlr*
CacheCntlr::lastLevelCache()
{
   if (! m_last_level) {
      /* Find last-level cache */
      CacheCntlr* last_level = this;
      while(last_level->m_next_cache_cntlr)
         last_level = last_level->m_next_cache_cntlr;
      m_last_level = last_level;
   }
   return m_last_level;
}

bool
CacheCntlr::isShared(core_id_t core_id)
{
   core_id_t core_id_master = core_id - core_id % m_shared_cores;
   return core_id_master == m_core_id_master;
}


/*** threads ***/

void
CacheCntlr::wakeUpUserThread(Semaphore* user_thread_sem)
{
   (user_thread_sem ? user_thread_sem : m_user_thread_sem)->signal();
}

void
CacheCntlr::waitForUserThread(Semaphore* network_thread_sem)
{
   (network_thread_sem ? network_thread_sem : m_network_thread_sem)->wait();
}

void
CacheCntlr::waitForNetworkThread()
{
   m_user_thread_sem->wait();
}

void
CacheCntlr::wakeUpNetworkThread()
{
   m_network_thread_sem->signal();
}

Semaphore*
CacheCntlr::getUserThreadSemaphore()
{
   return m_user_thread_sem;
}

Semaphore*
CacheCntlr::getNetworkThreadSemaphore()
{
   return m_network_thread_sem;
}

}
