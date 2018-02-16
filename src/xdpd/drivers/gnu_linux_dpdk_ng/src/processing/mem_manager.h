/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _MEM_MANAGER_H_
#define _MEM_MANAGER_H_

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

//C++ extern C
ROFL_BEGIN_DECLS

/**
 * Allocate memory pools
 */
rofl_result_t memory_init(
		unsigned int lcore_id,
		unsigned int mem_pool_size, unsigned int mbuf_dataroom,
		unsigned int direct_cache_size, unsigned int direct_priv_size,
		unsigned int indirect_cache_size, unsigned int indirect_priv_size);

/**
 * Deallocate memory pools
 */
rofl_result_t memory_destroy(unsigned int lcore_id);

//C++ extern C
ROFL_END_DECLS

#endif //_MEM_MANAGER_H_
