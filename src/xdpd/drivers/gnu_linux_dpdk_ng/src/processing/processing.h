/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _PROCESSING_H_
#define _PROCESSING_H_

#include <rofl_datapath.h>
#include "../config.h"
#include "../config_rss.h"
#include <rte_config.h> 
#include <rte_common.h> 
#include <rte_eal.h> 
#include <rte_log.h>
#include <rte_launch.h> 
#include <rte_mempool.h> 
#include <rte_mbuf.h> 
#include <rte_ethdev.h>
#include <rte_ring.h>
#include <rte_launch.h>

#include "../io/dpdk_datapacket.h"

#define PROCESSING_MAX_PORTS_PER_CORE 32
#define PROCESSING_MAX_PORTS 128 

typedef struct rx_port_queue {
	/* all these elemens in rxqueues are enabled by default */
	uint8_t port_id;
	uint8_t queue_id;
} __rte_cache_aligned rx_port_queue_t;

typedef struct tx_port_queue {
	uint8_t enabled;
	uint8_t queue_id;
	struct rte_mbuf *tx_pkts[PROC_ETH_TX_BURST_SIZE];
	unsigned int nb_tx_pkts;
} __rte_cache_aligned tx_port_queue_t;

#if 0
// Burst definition(queue)
struct mbuf_burst {
	unsigned len;
	struct rte_mbuf *burst[IO_IFACE_MAX_PKT_BURST];
};
#endif

#if 0 /* XXX(toanju) disable queues for now */
// Port queues
typedef struct port_bursts{
	//This are TX-queues of a port
	unsigned int core_id; //core id serving RX/TX on this port
	struct mbuf_burst tx_queues_burst[IO_IFACE_NUM_QUEUES];
}port_bursts_t;
#endif



/**
 * RX lcore task
 */
typedef struct rx_core_task {
	bool available; // task is runnable on lcore
	bool active; // task is running

	/*
	 * enqueuing on event device
	 */
	unsigned int socket_id; /* NUMA node socket-id */
	uint8_t ev_port_id; /* event port-id */
	uint8_t tx_ev_queue_id; /* event queue-id for transmitting events to worker cores */

	/*
	 * receiving from ethdevs
	 */
	rx_port_queue_t rx_queues[PROC_MAX_RX_QUEUES_PER_LCORE];  // (port_id, queue_id) = rx_queues[i] for i in (0...PROC_MAX_RX_QUEUES_PER_LCORE-1)
	uint16_t nb_rx_queues; // number of valid fields in rx_queues (0, nb_rx_queues-1)
} __rte_cache_aligned rx_core_task_t;

/**
 * TX lcore task
 */
typedef struct tx_core_task {
	bool available; // task is runnable on lcore
	bool active; // task is running

	/*
	 * dequeuing from event device
	 */
	unsigned int socket_id; /* NUMA node socket-id */
	uint8_t ev_port_id; /* event port-id */
	uint8_t rx_ev_queue_id; /* event queue-id for receiving events */

	/*
	 * transmitting to ethdevs
	 */
	tx_port_queue_t tx_queues[RTE_MAX_ETHPORTS]; // queue_id = tx_queues[port_id] => for all ports in the system
#if 0
	//These are the TX-queues for ALL ports in the system; index is port_id
	struct mbuf_burst tx_mbufs[RTE_MAX_ETHPORTS];
#endif
} __rte_cache_aligned tx_core_task_t;

/**
 * worker lcore task
 */
typedef struct wk_core_task {
	bool available; // task is runnable on lcore
	bool active; // task is running
	
	unsigned int socket_id; /* NUMA node socket-id */
	uint8_t ev_port_id; /* event port-id */
	uint8_t rx_ev_queue_id; /* event queue-id for receiving events */
	uint8_t tx_ev_queue_id[RTE_MAX_NUMA_NODES]; /* event queue-id for sending events to the appropriare TX lcore event queue */


	uint16_t n_rx_queue;
	rx_port_queue_t rx_queue_list[MAX_RX_QUEUE_PER_LCORE];
	uint16_t n_tx_port;
	uint8_t tx_queue_id[RTE_MAX_ETHPORTS]; // tx_queue_id[port_id] = queue_id => transmission queue for outgoing packets
	uint16_t tx_port_id[RTE_MAX_ETHPORTS];

#if 0
	//This are the TX-queues for ALL ports in the system; index is port_id
	struct mbuf_burst tx_mbufs[RTE_MAX_ETHPORTS];
#endif
} __rte_cache_aligned wk_core_task_t;

/**
* Processig core tasks 
*/
extern rx_core_task_t rx_core_tasks[RTE_MAX_LCORE];
extern tx_core_task_t tx_core_tasks[RTE_MAX_LCORE];
extern wk_core_task_t wk_core_tasks[RTE_MAX_LCORE];
extern struct rte_mempool* direct_pools[RTE_MAX_NUMA_NODES];
extern struct rte_mempool* indirect_pools[RTE_MAX_NUMA_NODES];
extern switch_port_t* port_list[PROCESSING_MAX_PORTS];
extern rte_spinlock_t spinlock_conf[RTE_MAX_ETHPORTS];

/**
* Total number of physical ports (scheduled, so usable by the I/O)
*/
extern unsigned int total_num_of_phy_ports;

/**
* Total number of NF ports (scheduled, so usable by the I/O)
*/
extern unsigned int total_num_of_nf_ports;

/**
* Running hash
*/
extern unsigned int running_hash; 


//C++ extern C
ROFL_BEGIN_DECLS

/**
* Initialize data structures for lcores
*/
rofl_result_t processing_init_lcores(void);

/**
* Initialize data structures for event device
*/
rofl_result_t processing_init_eventdev(void);

/**
* Initialize data structures for processing
*/
rofl_result_t processing_init(void);

/**
* Run processing lcores
*/
rofl_result_t processing_run(void);

/**
* Terminate processing lcores
*/
rofl_result_t processing_shutdown(void);

/**
* Deallocate data structures for processing
*/
rofl_result_t processing_destroy(void);

/**
* Schedule (physical) port to a core 
*/
rofl_result_t processing_schedule_port(switch_port_t* port);

/**
* Schedule NF port to a core 
*/
rofl_result_t processing_schedule_nf_port(switch_port_t* port);


/**
* Deschedule port to a core 
*/
rofl_result_t processing_deschedule_port(switch_port_t* port);

/**
* Deschedule NF port to a core 
*/
rofl_result_t processing_deschedule_nf_port(switch_port_t* port);


/**
* Packet processing routine for cores 
*/
int processing_core_process_packets(void*);

/**
 * RX packet reception
 */
int processing_packet_reception(void*);

/**
 * TX packet transmission
 */
int processing_packet_transmission(void*);

/**
* Dump core state
*/
void processing_dump_core_states(void);

//C++ extern C
ROFL_END_DECLS

#endif //_PROCESSING_H_
