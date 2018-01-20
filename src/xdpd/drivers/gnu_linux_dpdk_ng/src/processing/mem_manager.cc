#include <mem_manager.h>
#include <rofl_datapath.h>
extern "C" {
#include <rte_config.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_errno.h>
#include <utils/c_logger.h>
}

struct rte_mempool* direct_pools[RTE_MAX_NUMA_NODES];
struct rte_mempool* indirect_pools[RTE_MAX_NUMA_NODES];

/**
 * Allocate memory pools
 */
rofl_result_t memory_init(unsigned int socket_id, unsigned int mem_pool_size, unsigned int mbuf_dataroom){

	/* direct mbufs */
	if(direct_pools[socket_id] == NULL){

		/**
		*  create the mbuf pool for that socket id
		*/
		char pool_name[RTE_MEMPOOL_NAMESIZE];
		snprintf (pool_name, RTE_MEMPOOL_NAMESIZE, "pool_direct_%u", socket_id);
		XDPD_INFO(DRIVER_NAME"[memory][init] creating mempool %s with %u mbufs each of size %u bytes for CPU socket %u\n", pool_name, mem_pool_size, mbuf_dataroom, socket_id);

		direct_pools[socket_id] = rte_pktmbuf_pool_create(
				pool_name,
				/*number of elements in pool=*/mem_pool_size,
				/*cache_size=*/0,
				/*priv_size=*/RTE_ALIGN(sizeof(struct rte_pktmbuf_pool_private), RTE_MBUF_PRIV_ALIGN),
				/*data_room_size=*/mbuf_dataroom,
				socket_id);

		if (direct_pools[socket_id] == NULL) {
			XDPD_INFO(DRIVER_NAME"[memory][init] unable to allocate mempool %s due to error %u (%s)\n", pool_name, rte_errno, rte_strerror(rte_errno));
			rte_panic("Cannot initialize direct mbuf pool for CPU socket: %u\n", socket_id);
		}
	}

	/* indirect mbufs */
	if(indirect_pools[socket_id] == NULL){

		/**
		*  create the mbuf pool for that socket id
		*/
		char pool_name[RTE_MEMPOOL_NAMESIZE];
		snprintf (pool_name, RTE_MEMPOOL_NAMESIZE, "pool_indirect_%u", socket_id);
		XDPD_INFO(DRIVER_NAME"[memory][init] creating mempool %s with %u mbufs each of size %u bytes for CPU socket %u\n", pool_name, mem_pool_size, mbuf_dataroom, socket_id);

		indirect_pools[socket_id] = rte_pktmbuf_pool_create(
				pool_name,
				/*number of elements in pool=*/mem_pool_size,
				/*cache_size=*/0,
				/*priv_size=*/RTE_ALIGN(sizeof(struct rte_pktmbuf_pool_private), RTE_MBUF_PRIV_ALIGN),
				/*data_room_size=*/mbuf_dataroom,
				socket_id);

		if (indirect_pools[socket_id] == NULL) {
			XDPD_INFO(DRIVER_NAME"[memory][init] unable to allocate mempool %s due to error %u (%s)\n", pool_name, rte_errno, rte_strerror(rte_errno));
			rte_panic("Cannot initialize indirect mbuf pool for CPU socket: %u\n", socket_id);
		}
	}

	return ROFL_SUCCESS;
}

/**
 * Deallocate memory pools
 */
rofl_result_t memory_destroy(unsigned int socket_id){

	/* direct mbufs */
	if(direct_pools[socket_id] != NULL){
		//TODO
	}

	/* indirect mbufs */
	if(indirect_pools[socket_id] != NULL){
		//TODO
	}

	return ROFL_SUCCESS;
}
