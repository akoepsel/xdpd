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

extern struct rte_mempool* direct_pools[RTE_MAX_NUMA_NODES];
extern struct rte_mempool* indirect_pools[RTE_MAX_NUMA_NODES];

//C++ extern C
ROFL_BEGIN_DECLS

/**
 * Allocate memory pools
 */
rofl_result_t memory_init(unsigned int socket_id, unsigned int mem_pool_size, unsigned int mbuf_dataroom);

/**
 * Deallocate memory pools
 */
rofl_result_t memory_destroy(unsigned int socket_id);

//C++ extern C
ROFL_END_DECLS

#endif //_MEM_MANAGER_H_
