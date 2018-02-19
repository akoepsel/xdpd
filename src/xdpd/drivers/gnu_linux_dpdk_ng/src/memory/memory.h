/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _MEMORY_H_
#define _MEMORY_H_

#include <rofl_datapath.h>
#include "../config.h"
#include "../config_rss.h"
extern "C" {
#include <rte_config.h>
#include <rte_mempool.h>
}

typedef struct mempool_task {
	/* mempool is available for task */
	bool available;

	/* dpdk mempool itself */
	struct rte_mempool* pool_direct;
	struct rte_mempool* pool_indirect;
} mempool_task_t;

extern mempool_task_t mempool_per_task[RTE_MAX_LCORE];

/* memory pool for EAL (non-LCORE) activities */
extern struct rte_mempool* eal_mempool_direct[RTE_MAX_NUMA_NODES];
/* memory pool for EAL (non-LCORE) activities */
extern struct rte_mempool* eal_mempool_indirect[RTE_MAX_NUMA_NODES];


//C++ extern C
ROFL_BEGIN_DECLS

/**
 * Initialize memory subsystem
 */
rofl_result_t memory_init();

/**
 * Allocate memory pools
 */
rofl_result_t memory_allocate_lcore(
		unsigned int lcore_id,
		unsigned int mem_pool_size, unsigned int mbuf_dataroom,
		unsigned int direct_cache_size, unsigned int direct_priv_size,
		unsigned int indirect_cache_size, unsigned int indirect_priv_size);

/**
 * Deallocate memory pools
 */
rofl_result_t memory_deallocate_lcore(unsigned int lcore_id);

//C++ extern C
ROFL_END_DECLS

#endif //_MEMORY_H_
