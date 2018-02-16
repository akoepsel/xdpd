#include <mem_manager.h>
#include <rofl_datapath.h>
extern "C" {
#include <rte_config.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_errno.h>
#include <utils/c_logger.h>
}

mempool_task_t mempool_per_task[RTE_MAX_LCORE];

/**
 * Allocate memory pools
 */
rofl_result_t memory_init(
		unsigned int lcore_id, unsigned int mem_pool_size, unsigned int mbuf_dataroom,
		unsigned int direct_cache_size, unsigned int direct_priv_size,
		unsigned int indirect_cache_size, unsigned int indirect_priv_size){

	char pool_name[RTE_MEMPOOL_NAMESIZE];
	int socket_id = rte_lcore_to_socket_id(lcore_id);

	if(mempool_per_task[lcore_id].available == 0){

		/*
		 * direct mempool
		 */

		//cache_size = 16383; // 16383
		//priv_size = 32; //RTE_ALIGN(sizeof(struct rte_pktmbuf_pool_private), RTE_MBUF_PRIV_ALIGN); // 32

		/**
		*  create the mbuf pool for that lcore id
		*/
		snprintf (pool_name, RTE_MEMPOOL_NAMESIZE, "lcore_%u_direct", lcore_id);
		XDPD_INFO(DRIVER_NAME"[memory][init] lcore: %u => creating mempool %s with %u mbufs each of size %u bytes on socket %u, cache_size: %u, priv_size: %u\n",
				lcore_id, pool_name, mem_pool_size, mbuf_dataroom, socket_id, direct_cache_size, direct_priv_size);

		mempool_per_task[lcore_id].pool_direct = rte_pktmbuf_pool_create(
				pool_name,
				/*number of elements in pool=*/mem_pool_size,
				direct_cache_size,
				RTE_ALIGN(direct_priv_size, RTE_MBUF_PRIV_ALIGN),
				/*data_room_size=*/mbuf_dataroom,
				socket_id);

		if (mempool_per_task[lcore_id].pool_direct == NULL) {
			XDPD_INFO(DRIVER_NAME"[memory][init] unable to allocate mempool %s due to error %u (%s)\n", pool_name, rte_errno, rte_strerror(rte_errno));
			rte_panic("Cannot initialize direct mbuf pool for CPU socket: %u\n", socket_id);
		}

		/*
		 * indirect mempool
		 */

		//cache_size = sizeof(struct rte_mbuf);
		//priv_size = 32;

		/**
		*  create the mbuf pool for that lcore id
		*/
		snprintf (pool_name, RTE_MEMPOOL_NAMESIZE, "lcore_%u_indirect", lcore_id);
		XDPD_INFO(DRIVER_NAME"[memory][init] lcore: %u => creating mempool %s with %u mbufs each of size %u bytes on socket %u, cache_size: %u, priv_size: %u\n",
				lcore_id, pool_name, mem_pool_size, mbuf_dataroom, socket_id, indirect_cache_size, indirect_priv_size);

		mempool_per_task[lcore_id].pool_indirect = rte_pktmbuf_pool_create(
				pool_name,
				/*number of elements in pool=*/mem_pool_size,
				indirect_cache_size,
				RTE_ALIGN(indirect_priv_size, RTE_MBUF_PRIV_ALIGN),
				/*data_room_size=*/mbuf_dataroom,
				socket_id);

		if (mempool_per_task[lcore_id].pool_indirect == NULL) {
			XDPD_INFO(DRIVER_NAME"[memory][init] unable to allocate mempool %s due to error %u (%s)\n", pool_name, rte_errno, rte_strerror(rte_errno));
			rte_panic("Cannot initialize indirect mbuf pool for CPU socket: %u\n", socket_id);
		}

		/*
		 * memory pools (direct/indirect) successfully allocated for lcore_id
		 */
		mempool_per_task[lcore_id].available = 1;
	}

	return ROFL_SUCCESS;
}

/**
 * Deallocate memory pools
 */
rofl_result_t memory_destroy(unsigned int lcore_id){

	/* direct mbufs */
	if(mempool_per_task[lcore_id].pool_direct != NULL){
		rte_mempool_free(mempool_per_task[lcore_id].pool_direct);
		mempool_per_task[lcore_id].pool_direct = NULL;
	}

	/* indirect mbufs */
	if(mempool_per_task[lcore_id].pool_indirect != NULL){
		rte_mempool_free(mempool_per_task[lcore_id].pool_indirect);
		mempool_per_task[lcore_id].pool_indirect = NULL;
	}

	return ROFL_SUCCESS;
}
