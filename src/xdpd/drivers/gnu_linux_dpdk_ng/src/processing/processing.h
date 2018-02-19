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
#include <rte_ether.h>

#include <map>
#include <set>
#include <string>

#include "../memory/memory.h"
#include "../io/dpdk_datapacket.h"

typedef struct rx_ethdev_port_queue {
	/* all these elements in rxqueues are enabled by default */
	uint8_t up;
	/* ethdev port */
	uint8_t port_id;
	/* ethdev queue */
	uint8_t queue_id;
	/* associated worker lcore (=ev_queue_id) */
	uint8_t ev_queue_id;
} __rte_cache_aligned rx_ethdev_port_queue_t;

typedef struct tx_ethdev_port_queue {
	uint8_t enabled;
	/* all these elements in txqueues are enabled by default */
	uint8_t up;
	/* ethdev port */
	uint8_t port_id;
	/* ethdev queue */
	uint8_t queue_id;
	/* associated worker lcore (=ev_queue_id) */
	uint8_t ev_queue_id;
} __rte_cache_aligned tx_ethdev_port_queue_t;

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

/* rwlock for eventdev port used by control plane threads */
extern rte_rwlock_t rwlock_eventdev_cp_port;

enum event_queue_t {
	EVENT_QUEUE_TO_WK = 0,
	EVENT_QUEUE_TO_TX = 1,
	EVENT_QUEUE_MAX,
};

/**
 * RX lcore task
 */
typedef struct rx_core_task {
	bool available; // task is runnable on lcore
	bool active; // task is running
	uint8_t ev_port_id; // eventdev port
	unsigned int socket_id; // NUMA socket id
	task_statistics_t stats;

	/* Idea:
	 * We have a number of worker lcores on this NUMA socket. Each worker lcore has assigned
	 * a port/queue pair on all ethernet devices with a dedicated mempool. We forward packets
	 * received on this port/queue to a single worker lcore to avoid invalidation of memory
	 * caches.
	 */
	rx_ethdev_port_queue_t rx_queues[RTE_MAX_QUEUES_PER_PORT];  // (port_id, queue_id) = rx_queues[i] for i in (0...RTE_MAX_ETHPORTS-1)
	uint16_t nb_rx_queues; // number of valid fields in rx_queues (0, nb_rx_queues-1)

} __rte_cache_aligned rx_core_task_t;

/**
 * TX lcore task
 */
typedef struct tx_core_task {
	bool available; // task is runnable on lcore
	bool active; // task is running
	uint8_t ev_port_id; // eventdev port
	unsigned int socket_id; // NUMA socket id
	task_statistics_t stats;

	/*
	 * transmitting to ethdevs
	 */
	/* queue-id to be used by this task for given port-id */
	tx_ethdev_port_queue_t tx_queues[RTE_MAX_QUEUES_PER_PORT]; // tx_queues[ev_queue_id] => bound to event queues
	uint16_t nb_tx_queues; // number of valid fields in tx_queues (0, nb_tx_queues-1)

	/*
	 * dequeuing from event device
	 */

	/* list of event queues this task is linked to for receiving events */
	uint8_t rx_ev_queues[RTE_EVENT_MAX_QUEUES_PER_DEV];
	/* number of event queues stored in ex_ev_queues */
	unsigned int nb_rx_ev_queues;

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

} __rte_cache_aligned tx_core_task_t;

/**
 * worker lcore task
 */
typedef struct wk_core_task {
	bool available; // task is runnable on lcore
	bool active; // task is running
	task_statistics_t stats;
	
	/* NUMA node socket-id */
	unsigned int socket_id;
	/* event port-id */
	uint8_t ev_port_id;
	/* event queue-id for receiving events from RX tasks */
	uint8_t rx_ev_queue_id;
	/* event queue-id for sending events to TX tasks */
	uint8_t tx_ev_queue_id;

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

} __rte_cache_aligned ev_core_task_t;

/**
* Processing tasks: receive, transmit, worker
*/
/* rx tasks */
extern rx_core_task_t rx_core_tasks[RTE_MAX_LCORE];
/* tx tasks */
extern tx_core_task_t tx_core_tasks[RTE_MAX_LCORE];
/* wk tasks */
extern wk_core_task_t wk_core_tasks[RTE_MAX_LCORE];
/* ev tasks */
extern ev_core_task_t ev_core_tasks[RTE_MAX_NUMA_NODES];
/* portlist */
extern switch_port_t* port_list[PROCESSING_MAX_PORTS];
/* portlist rwlock */
extern rte_rwlock_t port_list_rwlock;
/* deprecated spnlock */
extern rte_spinlock_t spinlock_conf[RTE_MAX_ETHPORTS];

/* a set of available NUMA sockets (socket_id) */
extern std::set<int> numa_nodes;
/* a map of available event logical cores per NUMA socket (set of lcore_id) */
extern std::map<unsigned int, std::set<unsigned int> > ev_lcores;
/* a map of available RX logical cores per NUMA socket (set of lcore_id) */
extern std::map<unsigned int, std::set<unsigned int> > rx_lcores;
/* a map of available TX logical cores per NUMA socket (set of lcore_id) */
extern std::map<unsigned int, std::set<unsigned int> > tx_lcores;
/* a map of available worker logical cores per NUMA socket (set of lcore_id) */
extern std::map<unsigned int, std::set<unsigned int> > wk_lcores;



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

/**
 * Swap ethernet mac addresses for testing
 */
void l2fwd_swap_ether_addrs(struct rte_mbuf *m);

//C++ extern C
ROFL_END_DECLS

#endif //_PROCESSING_H_
