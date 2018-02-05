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
#include <rte_rwlock.h>
#include <rte_eventdev.h>

#include <string>

#include "../io/dpdk_datapacket.h"

typedef struct rx_port_queue {
	/* all these elemens in rxqueues are enabled by default */
	uint8_t port_id;
	uint8_t queue_id;
} __rte_cache_aligned rx_port_queue_t;

typedef struct tx_port_queue {
	uint8_t enabled;
	uint8_t queue_id;
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

typedef struct task_statistics {
	uint64_t rx_pkts;
	uint64_t tx_pkts;
	uint64_t rx_evts;
	uint64_t tx_evts;
	uint64_t evts_dropped; // number of packets dropped in outgoing eventdev
	uint64_t bugs_dropped; // number of packets dropped in tx-task due to misbehaving internal operations (also called bugs ;)
	uint64_t ring_dropped; // number of packets dropped in outgoing txring
	uint64_t eths_dropped; // number of packets dropped in outgoing etherdev
} __rte_cache_aligned task_statistics_t;


/**
 * RX lcore task
 */
typedef struct rx_core_task {
	bool available; // task is runnable on lcore
	bool active; // task is running
	task_statistics_t stats;

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
	task_statistics_t stats;
	bool lead_task; // true: first tx-task on this NUMA socket (will never be switched off in idle phases)
	unsigned int idle_loops; // number of idle loops for reading events from event device

	/*
	 * dequeuing from event device
	 */
	unsigned int socket_id; /* NUMA node socket-id */
	uint8_t ev_port_id; /* event port-id */
	uint8_t rx_ev_queue_id; /* event queue-id for receiving events */

	/*
	 * drain queues per port
	 */
	/* queues per port for storing packets before initiating tx-burst to eth-dev */
	struct rte_ring *txring[RTE_MAX_ETHPORTS];

	/* maximum number of packets allowed in queue before initiating tx-burst for port */
	unsigned int txring_drain_threshold[RTE_MAX_ETHPORTS];
	/* maximum packet capacity in drain queue */
	unsigned int txring_drain_queue_capacity[RTE_MAX_ETHPORTS];

	/* maximum time interval before initiating next tx-burst for port */
	uint64_t txring_drain_interval[RTE_MAX_ETHPORTS];
	/* timestamp of last tx-burst */
	uint64_t txring_last_tx_time[RTE_MAX_ETHPORTS];

	/*
	 * transmitting to ethdevs
	 */
	/* queue-id to be used by this task for given port-id */
	tx_port_queue_t tx_queues[RTE_MAX_ETHPORTS]; // queue_id = tx_queues[port_id] => for all ports in the system
	uint16_t nb_tx_queues; // number if valid tx_queues

} __rte_cache_aligned tx_core_task_t;

/**
 * worker lcore task
 */
typedef struct wk_core_task {
	bool available; // task is runnable on lcore
	bool active; // task is running
	task_statistics_t stats;
	
	unsigned int socket_id; /* NUMA node socket-id */
	uint8_t ev_port_id; /* event port-id */
	uint8_t rx_ev_queue_id; /* event queue-id for receiving events */
	uint8_t tx_ev_queue_id; /* event queue-id for sending events to the appropriate TX lcore event queue */

} __rte_cache_aligned wk_core_task_t;


/**
 * event device
 */
typedef struct ev_core_task {
	bool available; // task is runnable on lcore
	bool active; // task is running

	/* event device name */
	char name[64];
	/* event device arguments */
	char args[128];
	/* event device handle */
	uint8_t eventdev_id;
	/* event device info structure */
	struct rte_event_dev_info eventdev_info;
	/* event device configuration */
	struct rte_event_dev_config eventdev_conf;

	/* event queue forwarding events to worker tasks */
	uint8_t ev_queue_to_wk_tasks;
	/* event queue forwarding events to TX tasks */
	uint8_t ev_queue_to_tx_tasks;
} __rte_cache_aligned ev_core_task_t;

/**
* Processing tasks: receive, transmit, worker
*/
extern rx_core_task_t rx_core_tasks[RTE_MAX_LCORE];
extern tx_core_task_t tx_core_tasks[RTE_MAX_LCORE];
extern wk_core_task_t wk_core_tasks[RTE_MAX_LCORE];
extern ev_core_task_t ev_core_tasks[RTE_MAX_LCORE];
extern struct rte_mempool* direct_pools[RTE_MAX_NUMA_NODES];
extern struct rte_mempool* indirect_pools[RTE_MAX_NUMA_NODES];
extern switch_port_t* port_list[PROCESSING_MAX_PORTS];
extern rte_rwlock_t port_list_rwlock;
extern rte_spinlock_t spinlock_conf[RTE_MAX_ETHPORTS];
extern ev_core_task_t* eventdevs[RTE_MAX_NUMA_NODES];


/* maximum number of event queues per NUMA node: queue[0]=used by workers, queue[1]=used by TX lcores */
enum event_queue_t {
	EVENT_QUEUE_WK_TASKS = 0, //workers
	EVENT_QUEUE_TX_TASKS = 1, //TX cores
	EVENT_QUEUE_MAX = 2, /* max number of event queues per NUMA node */
};


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
* Allocate memory
*/
rofl_result_t processing_init_task_structures(void);

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
* Update processing task statistics
*/
void processing_update_stats(void);

/**
* Packet processing routine for cores 
*/
int processing_packet_pipeline_processing(void*);

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
