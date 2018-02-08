#include "processing.h"
#include <utils/c_logger.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_spinlock.h>
#include <rte_rwlock.h>
#include <rte_eventdev.h>
#include <rte_bus_vdev.h>
#include <rte_service.h>
#include <rte_log.h>
#include <sstream>
#include <iomanip>
#include "assert.h"
#include "../config_rss.h"
#include "../util/compiler_assert.h"
#include "../io/rx.h"
#include "../io/tx.h"

#include "../io/port_state.h"
#include "../io/iface_manager.h"
#include <rofl/datapath/pipeline/openflow/of_switch.h>

#include <set>
#include <cctype>
#include <algorithm>
#include <yaml-cpp/yaml.h>

extern YAML::Node y_config_dpdk_ng;

using namespace xdpd::gnu_linux_dpdk_ng;


//Number of MBUFs per pool (per CPU socket)
unsigned int mem_pool_size = 0;
unsigned int mbuf_dataroom = RTE_MBUF_DEFAULT_DATAROOM;
unsigned int mbuf_buf_size = RTE_MBUF_DEFAULT_BUF_SIZE;
unsigned int dpdk_memory_mempool_direct_cache_size = 16383;
unsigned int dpdk_memory_mempool_direct_priv_size = 32;
unsigned int dpdk_memory_mempool_indirect_cache_size = sizeof(struct rte_mbuf);
unsigned int dpdk_memory_mempool_indirect_priv_size = 32;
unsigned int max_eth_rx_burst_size = MAX_ETH_RX_BURST_SIZE_DEFAULT;
unsigned int max_evt_wk_burst_size = MAX_EVT_WK_BURST_SIZE_DEFAULT;
unsigned int max_evt_tx_burst_size = MAX_EVT_TX_BURST_SIZE_DEFAULT;
unsigned int max_eth_tx_burst_size = MAX_ETH_TX_BURST_SIZE_DEFAULT;
/* shortcutting the openflow pipeline => for testing pure I/O performance including eventdev subsystem */
bool pipeline_shortcut = false;
/* shortcutting the eventdev subsystem => for testing raw I/O performance excluding eventdev subsystem */
bool eventdev_shortcut = false;

const std::string eventdev_args_default("sched_quanta=64,credit_quanta=32");

//
// Processing state
//
static unsigned int max_cores;


static unsigned total_num_of_ports = 0;

switch_port_t* port_list[PROCESSING_MAX_PORTS];
rte_rwlock_t port_list_rwlock;

/*
 * lcore task structures
 */
/* tasks running on RX lcores */
rx_core_task_t rx_core_tasks[RTE_MAX_LCORE];
/* tasks running on TX lcores */
tx_core_task_t tx_core_tasks[RTE_MAX_LCORE];
/* tasks running on worker lcores */
wk_core_task_t wk_core_tasks[RTE_MAX_LCORE];
/* tasks running on event lcores */
ev_core_task_t ev_core_tasks[RTE_MAX_LCORE];

/*
 * lcore related parameters
 */
/* a set of available NUMA sockets (socket_id) */
std::set<int> numa_nodes;
/* a map of available event logical cores per NUMA socket (set of lcore_id) */
std::map<unsigned int, std::set<unsigned int> > ev_lcores;
/* a map of available RX logical cores per NUMA socket (set of lcore_id) */
std::map<unsigned int, std::set<unsigned int> > rx_lcores;
/* a map of available TX logical cores per NUMA socket (set of lcore_id) */
std::map<unsigned int, std::set<unsigned int> > tx_lcores;
/* a map of available worker logical cores per NUMA socket (set of lcore_id) */
std::map<unsigned int, std::set<unsigned int> > wk_lcores;

/* service lcores */
static uint64_t ev_coremask = 0x0002; // lcore_id = 1
/* RX lcores */
static uint64_t rx_coremask = 0x0004; // lcore_id = 2
/* TX lcores */
static uint64_t tx_coremask = 0x0008; // lcore_id = 3
/* WK lcores */
static uint64_t wk_coremask = 0x000c; // lcore_id = 4

/* event devs */
ev_core_task_t* eventdevs[RTE_MAX_NUMA_NODES];


/*
* Initialize data structures for lcores
*/
rofl_result_t processing_init_lcores(void){

	int ret;

	//Initialize logical core structure: all lcores disabled
	for (int j = 0; j < RTE_MAX_LCORE; j++) {
		lcores[j].socket_id = -1;
		lcores[j].is_master = 0;
		lcores[j].is_enabled = 0;
		lcores[j].next_lcore_id = -1;
		lcores[j].is_wk_lcore = 0;
		lcores[j].is_rx_lcore = 0;
		lcores[j].is_tx_lcore = 0;
		lcores[j].is_ev_lcore = 0;
	}
	numa_nodes.clear();
	rx_lcores.clear();
	tx_lcores.clear();
	wk_lcores.clear();
	ev_lcores.clear();

	//Get master lcore
	unsigned int master_lcore_id = rte_get_master_lcore();

	/* number of mbufs to allocate per NUMA socket
	 * if unspecified, calculate automatically */
	YAML::Node mem_pool_size_node = y_config_dpdk_ng["dpdk"]["memory"]["mem_pool_size"];
	if (mem_pool_size_node && mem_pool_size_node.IsScalar()) {
		mem_pool_size = mem_pool_size_node.as<unsigned int>();
	}

	/* mbuf data room size = size per packet (default: 2048) */
	YAML::Node mbuf_dataroom_node = y_config_dpdk_ng["dpdk"]["memory"]["mbuf_dataroom"];
	if (mbuf_dataroom_node && mbuf_dataroom_node.IsScalar()) {
		mbuf_dataroom = mbuf_dataroom_node.as<unsigned int>();
		mbuf_buf_size = mbuf_dataroom + RTE_PKTMBUF_HEADROOM;
	}

	/* dpdk.memory.mempool.direct.cache_size */
	YAML::Node dpdk_memory_mempool_direct_cache_size_node = y_config_dpdk_ng["dpdk"]["memory"]["mempool"]["direct"]["cache_size"];
	if (dpdk_memory_mempool_direct_cache_size_node && dpdk_memory_mempool_direct_cache_size_node.IsScalar()) {
		dpdk_memory_mempool_direct_cache_size = dpdk_memory_mempool_direct_cache_size_node.as<unsigned int>();
	}

	/* dpdk.memory.mempool.direct.priv_size */
	YAML::Node dpdk_memory_mempool_direct_priv_size_node = y_config_dpdk_ng["dpdk"]["memory"]["mempool"]["direct"]["priv_size"];
	if (dpdk_memory_mempool_direct_priv_size_node && dpdk_memory_mempool_direct_priv_size_node.IsScalar()) {
		dpdk_memory_mempool_direct_priv_size = dpdk_memory_mempool_direct_priv_size_node.as<unsigned int>();
	}

	/* dpdk.memory.mempool.indirect.cache_size */
	YAML::Node dpdk_memory_mempool_indirect_cache_size_node = y_config_dpdk_ng["dpdk"]["memory"]["mempool"]["indirect"]["cache_size"];
	if (dpdk_memory_mempool_indirect_cache_size_node && dpdk_memory_mempool_indirect_cache_size_node.IsScalar()) {
		dpdk_memory_mempool_indirect_cache_size = dpdk_memory_mempool_indirect_cache_size_node.as<unsigned int>();
	}

	/* dpdk.memory.mempool.indirect.priv_size */
	YAML::Node dpdk_memory_mempool_indirect_priv_size_node = y_config_dpdk_ng["dpdk"]["memory"]["mempool"]["indirect"]["priv_size"];
	if (dpdk_memory_mempool_indirect_priv_size_node && dpdk_memory_mempool_indirect_priv_size_node.IsScalar()) {
		dpdk_memory_mempool_indirect_priv_size = dpdk_memory_mempool_indirect_priv_size_node.as<unsigned int>();
	}

	/* get svc coremask */
	YAML::Node svc_coremask_node = y_config_dpdk_ng["dpdk"]["lcores"]["svc_coremask"];
	if (svc_coremask_node && svc_coremask_node.IsScalar()) {
		ev_coremask = svc_coremask_node.as<uint64_t>();
	}

	/* get rx coremask */
	YAML::Node rx_coremask_node = y_config_dpdk_ng["dpdk"]["lcores"]["rx_coremask"];
	if (rx_coremask_node && rx_coremask_node.IsScalar()) {
		rx_coremask = rx_coremask_node.as<uint64_t>();
	}

	/* get tx coremask */
	YAML::Node tx_coremask_node = y_config_dpdk_ng["dpdk"]["lcores"]["tx_coremask"];
	if (tx_coremask_node && tx_coremask_node.IsScalar()) {
		tx_coremask = tx_coremask_node.as<uint64_t>();
	}

	/* get wk coremask */
	YAML::Node wk_coremask_node = y_config_dpdk_ng["dpdk"]["lcores"]["wk_coremask"];
	if (wk_coremask_node && wk_coremask_node.IsScalar()) {
		wk_coremask = wk_coremask_node.as<uint64_t>();
	}

	/* get max_eth_rx_burst_size */
	YAML::Node max_eth_rx_burst_size_node = y_config_dpdk_ng["dpdk"]["processing"]["max_eth_rx_burst_size"];
	if (max_eth_rx_burst_size_node && max_eth_rx_burst_size_node.IsScalar()) {
		max_eth_rx_burst_size = max_eth_rx_burst_size_node.as<unsigned int>();
	}
	XDPD_INFO(DRIVER_NAME"[processing][init] max_eth_rx_burst_size=%u\n", max_eth_rx_burst_size);

	/* get max_evt_proc_burst_size */
	YAML::Node max_evt_wk_burst_size_node = y_config_dpdk_ng["dpdk"]["processing"]["max_evt_wk_burst_size"];
	if (max_evt_wk_burst_size_node && max_evt_wk_burst_size_node.IsScalar()) {
		max_evt_wk_burst_size = max_evt_wk_burst_size_node.as<unsigned int>();
	}
	XDPD_INFO(DRIVER_NAME"[processing][init] max_evt_wk_burst_size=%u\n", max_evt_wk_burst_size);

	/* get max_evt_tx_burst_size */
	YAML::Node max_evt_tx_burst_size_node = y_config_dpdk_ng["dpdk"]["processing"]["max_evt_tx_burst_size"];
	if (max_evt_tx_burst_size_node && max_evt_tx_burst_size_node.IsScalar()) {
		max_evt_tx_burst_size = max_evt_tx_burst_size_node.as<unsigned int>();
	}
	XDPD_INFO(DRIVER_NAME"[processing][init] max_evt_tx_burst_size=%u\n", max_evt_tx_burst_size);

	/* get max_eth_tx_burst_size */
	YAML::Node max_eth_tx_burst_size_node = y_config_dpdk_ng["dpdk"]["processing"]["max_eth_tx_burst_size"];
	if (max_eth_tx_burst_size_node && max_eth_tx_burst_size_node.IsScalar()) {
		max_eth_tx_burst_size = max_eth_tx_burst_size_node.as<unsigned int>();
	}
	XDPD_INFO(DRIVER_NAME"[processing][init] max_eth_tx_burst_size=%u\n", max_eth_tx_burst_size);

	/* enable pipeline shortcut */
	YAML::Node pipeline_shortcut_node = y_config_dpdk_ng["dpdk"]["processing"]["pipeline_shortcut"]["pipeline"];
	if (pipeline_shortcut_node && pipeline_shortcut_node.IsScalar()) {
		pipeline_shortcut = pipeline_shortcut_node.as<bool>();
	}
	XDPD_INFO(DRIVER_NAME"[processing][init] pipeline_shortcut=%u\n", pipeline_shortcut);

	/* enable eventdev shortcut */
	YAML::Node eventdev_shortcut_node = y_config_dpdk_ng["dpdk"]["processing"]["eventdev_shortcut"]["eventdev"];
	if (eventdev_shortcut_node && eventdev_shortcut_node.IsScalar()) {
		eventdev_shortcut = eventdev_shortcut_node.as<bool>();
	}
	XDPD_INFO(DRIVER_NAME"[processing][init] eventdev_shortcut=%u\n", eventdev_shortcut);

	/* detect all lcores and their state */
	for (unsigned int lcore_id = 0; lcore_id < rte_lcore_count(); lcore_id++) {
		if (lcore_id >= RTE_MAX_LCORE) {
			continue;
		}
		unsigned int socket_id = rte_lcore_to_socket_id(lcore_id);

		numa_nodes.insert(socket_id);

		lcores[lcore_id].socket_id = socket_id;
		lcores[lcore_id].is_enabled = rte_lcore_is_enabled(lcore_id);

		/* get next lcore */
		unsigned int next_lcore_id = RTE_MAX_LCORE;
		if ((next_lcore_id = rte_get_next_lcore(lcore_id, /*skip-master=*/1, /*wrap=*/1)) < RTE_MAX_LCORE) {
			lcores[lcore_id].next_lcore_id = next_lcore_id;
		}

		/* get lcore's role */
		enum rte_lcore_role_t role = rte_eal_lcore_role(lcore_id);
		switch (role) {
		case ROLE_OFF: {
			/* skip node, i.e.: do nothing */
			XDPD_INFO(DRIVER_NAME"[processing][init][lcores] skipping lcore: %3u on socket: %2u, role: OFF\n", lcore_id, socket_id);
		} break;
		case ROLE_SERVICE: {
			/* skip node, i.e.: do nothing */
			XDPD_INFO(DRIVER_NAME"[processing][init][lcores] skipping lcore: %3u on socket: %2u, role: SERVICE\n", lcore_id, socket_id);
		} break;
		case ROLE_RTE: {

			std::string s_task;

			//master lcore?
			if (lcore_id == master_lcore_id) {
				lcores[lcore_id].is_master = 1;
				s_task.assign("master lcore");
			} else
			//ev lcore (=service cores)
			if (ev_coremask & ((uint64_t)1 << lcore_id)) {
				if ((ret = rte_service_lcore_add(lcore_id)) < 0) {
					switch (ret) {
					case -EALREADY: {
						/* do nothing */
					} break;
					default: {
						XDPD_ERR(DRIVER_NAME"[processing][init][lcores] adding lcore %3u to service cores failed\n", lcore_id);
						return ROFL_FAILURE;
					};
					}
				}
				lcores[lcore_id].is_ev_lcore = 1;
				//Increase number of service lcores for this socket
				ev_lcores[socket_id].insert(lcore_id);
				s_task.assign("service lcore");
				//eventdev on NUMA node socket_id
				eventdevs[socket_id] = &ev_core_tasks[lcore_id];
			} else
			//rx lcore (=packet receiving lcore)
			if (rx_coremask & ((uint64_t)1 << lcore_id)) {
				lcores[lcore_id].is_rx_lcore = 1;
				//Increase number of RX lcores for this socket
				rx_lcores[socket_id].insert(lcore_id);
				s_task.assign("rx lcore");
			} else
			//tx lcore (=packet transmitting lcore)
			if (tx_coremask & ((uint64_t)1 << lcore_id)) {
				lcores[lcore_id].is_tx_lcore = 1;
				//Increase number of TX lcores for this socket
				tx_lcores[socket_id].insert(lcore_id);
				s_task.assign("tx lcore");
			} else
			//wk lcore (=worker running openflow pipeline)
			if (wk_coremask & ((uint64_t)1 << lcore_id)) {
				lcores[lcore_id].is_wk_lcore = 1;
				//Increase number of worker lcores for this socket
				wk_lcores[socket_id].insert(lcore_id);
				s_task.assign("wk lcore");
			} else
			{
				s_task.assign("unused lcore");
			}

			XDPD_INFO(DRIVER_NAME"[processing][init][lcores] adding lcore: %3u on socket: %2u, enabled: %s, task: %s, next lcore is: %3u, #working lcores on socket(%u): %u\n",
					lcore_id,
					socket_id,
					(lcores[lcore_id].is_enabled) ? "yes" : "no",
					s_task.c_str(),
					lcores[lcore_id].next_lcore_id,
					socket_id,
					wk_lcores[socket_id].size());

		} break;
		default: {

		}
		}
	}

	return ROFL_SUCCESS;
}

/**
* Allocate memory
*/
rofl_result_t processing_init_task_structures(void) {

	bool lead_task[RTE_MAX_NUMA_NODES];

	for (unsigned int i = 0; i < RTE_MAX_NUMA_NODES; i++) {
		lead_task[i] = true;
	}

	//Define available cores
	for (unsigned int lcore_id = 0; lcore_id < rte_lcore_count(); lcore_id++) {
		enum rte_lcore_role_t role = rte_eal_lcore_role(lcore_id);
		if(role == ROLE_RTE){

			//Recover CPU socket for the lcore
			unsigned int socket_id = rte_lcore_to_socket_id(lcore_id);

			if (lcores[lcore_id].is_master){
				continue;
			}
			if (lcores[lcore_id].is_ev_lcore) {
				ev_core_tasks[lcore_id].available = true;
				continue;
			}
			if (lcores[lcore_id].is_wk_lcore) {
				wk_core_tasks[lcore_id].available = true;
				continue;
			}
			if (lcores[lcore_id].is_tx_lcore) {
				tx_core_tasks[lcore_id].available = true;
				tx_core_tasks[lcore_id].lead_task = lead_task[socket_id]; //the first core on a socket is the lead-task, that will never stop in idle phases
				lead_task[socket_id] = false;
				continue;
			}
			if (lcores[lcore_id].is_rx_lcore) {
				rx_core_tasks[lcore_id].available = true;
				continue;
			}

			//XDPD_DEBUG(DRIVER_NAME"[processing][init] marking core %u as available\n", lcore_id);
		}
	}

	return ROFL_SUCCESS;
}

/*
* Initialize data structures for RTE event device
*/
rofl_result_t processing_init_eventdev(void){

	int ret;

	for (auto socket_id : numa_nodes) {
		for (auto lcore_id : ev_lcores[socket_id]) {
			/*
			 * initialize eventdev device
			 */
			XDPD_DEBUG(DRIVER_NAME"[processing][init][evdev] initializing eventdev device\n");

			/* get software event name */
			snprintf(ev_core_tasks[lcore_id].name, sizeof(ev_core_tasks[lcore_id].name), "event_sw%u", socket_id);

			/* get software event arguments */
			YAML::Node eventdev_args_node = y_config_dpdk_ng["dpdk"]["eventdev"]["args"];
			if (eventdev_args_node && eventdev_args_node.IsScalar()) {
				snprintf(ev_core_tasks[lcore_id].args, sizeof(ev_core_tasks[lcore_id].args), eventdev_args_node.as<std::string>().c_str());
			} else {
				snprintf(ev_core_tasks[lcore_id].args, sizeof(ev_core_tasks[lcore_id].args), eventdev_args_default.c_str());
			}

			/* initialize software event pmd */
			if ((ret = rte_vdev_init(ev_core_tasks[lcore_id].name, ev_core_tasks[lcore_id].args)) < 0) {
				switch (ret) {
				case -EINVAL: {
					XDPD_ERR(DRIVER_NAME"[processing][init][evdev] initialization of eventdev %s with args \"%s\" failed (EINVAL)\n", ev_core_tasks[lcore_id].name, ev_core_tasks[lcore_id].args);
				} break;
				case -EEXIST: {
					XDPD_ERR(DRIVER_NAME"[processing][init][evdev] initialization of eventdev %s with args \"%s\" failed (EEXIST)\n", ev_core_tasks[lcore_id].name, ev_core_tasks[lcore_id].args);
				} break;
				case -ENOMEM: {
					XDPD_ERR(DRIVER_NAME"[processing][init][evdev] initialization of eventdev %s with args \"%s\" failed (ENOMEM)\n", ev_core_tasks[lcore_id].name, ev_core_tasks[lcore_id].args);
				} break;
				default: {
					XDPD_ERR(DRIVER_NAME"[processing][init][evdev] initialization of eventdev %s with args \"%s\" failed\n", ev_core_tasks[lcore_id].name, ev_core_tasks[lcore_id].args);
				};
				}
				return ROFL_FAILURE;
			}

			XDPD_DEBUG(DRIVER_NAME"[processing][init][evdev] %u eventdev device(s) available\n", rte_event_dev_count());

			/* get eventdev id */
			ev_core_tasks[lcore_id].eventdev_id = rte_event_dev_get_dev_id(ev_core_tasks[lcore_id].name);

			/* get eventdev info structure */
			if ((ret = rte_event_dev_info_get(ev_core_tasks[lcore_id].eventdev_id, &ev_core_tasks[lcore_id].eventdev_info)) < 0) {
				XDPD_ERR(DRIVER_NAME"[processing][init][evdev] unable to retrieve info struct for eventdev %s\n", ev_core_tasks[lcore_id].name);
			}

			XDPD_DEBUG(DRIVER_NAME"[processing][init][evdev] eventdev: %s, max_event_ports: %u, max_event_queues: %u, max_num_events: %u\n",
					ev_core_tasks[lcore_id].name,
					ev_core_tasks[lcore_id].eventdev_info.max_event_ports,
					ev_core_tasks[lcore_id].eventdev_info.max_event_queues,
					ev_core_tasks[lcore_id].eventdev_info.max_num_events);


			/* configure event device */
			memset(&ev_core_tasks[lcore_id].eventdev_conf, 0, sizeof(ev_core_tasks[lcore_id].eventdev_conf));

			//number of event queues: number of RX tasks + number of WK tasks + number of control plane tasks
			ev_core_tasks[lcore_id].eventdev_conf.nb_event_queues = rx_lcores[socket_id].size() + wk_lcores[socket_id].size() + /*control plane*/1;
			ev_core_tasks[lcore_id].eventdev_conf.nb_event_ports =
                                            + rx_lcores[socket_id].size() /* number of all RX lcores on NUMA node socket_id */
                                            + wk_lcores[socket_id].size() /* number of all WK lcores on NUMA node socket_id */
                                            + tx_lcores[socket_id].size() /* number of all TX lcores on NUMA node socket_id */
                                            + 1;/* port_id=0 is reserved for Packet-Out from control plane */

			if (ev_core_tasks[lcore_id].eventdev_conf.nb_event_ports > ev_core_tasks[lcore_id].eventdev_info.max_event_ports) {
				XDPD_ERR(DRIVER_NAME"[processing][init][evdev] initialization of eventdev %s failed, too many event ports required\n", ev_core_tasks[lcore_id].name);
				return ROFL_FAILURE;
			}
			ev_core_tasks[lcore_id].eventdev_conf.nb_events_limit = ev_core_tasks[lcore_id].eventdev_info.max_num_events;
			ev_core_tasks[lcore_id].eventdev_conf.nb_event_queue_flows = ev_core_tasks[lcore_id].eventdev_info.max_event_queue_flows;
			ev_core_tasks[lcore_id].eventdev_conf.nb_event_port_dequeue_depth = ev_core_tasks[lcore_id].eventdev_info.max_event_port_dequeue_depth;
			ev_core_tasks[lcore_id].eventdev_conf.nb_event_port_enqueue_depth = ev_core_tasks[lcore_id].eventdev_info.max_event_port_enqueue_depth;

			XDPD_DEBUG(DRIVER_NAME"[processing][init][evdev] configuring eventdev: %s, nb_event_queues: %u, nb_event_ports: %u, nb_events_limit: %u, nb_event_queue_flows: %u, nb_event_port_dequeue_depth: %u, nb_event_port_enqueue_depth: %u\n",
					ev_core_tasks[lcore_id].name,
					ev_core_tasks[lcore_id].eventdev_conf.nb_event_queues,
					ev_core_tasks[lcore_id].eventdev_conf.nb_event_ports,
					ev_core_tasks[lcore_id].eventdev_conf.nb_events_limit,
					ev_core_tasks[lcore_id].eventdev_conf.nb_event_queue_flows,
					ev_core_tasks[lcore_id].eventdev_conf.nb_event_port_dequeue_depth,
					ev_core_tasks[lcore_id].eventdev_conf.nb_event_port_enqueue_depth);

			if ((ret = rte_event_dev_configure(ev_core_tasks[lcore_id].eventdev_id, &ev_core_tasks[lcore_id].eventdev_conf)) < 0) {
				XDPD_ERR(DRIVER_NAME"[processing][init][evdev] eventdev %s, rte_event_dev_configure() failed\n", ev_core_tasks[lcore_id].name);
				return ROFL_FAILURE;
			}


			/* configure event queues */
			for (unsigned int queue_id = 0; queue_id < ev_core_tasks[lcore_id].eventdev_conf.nb_event_queues; queue_id++) {
				struct rte_event_queue_conf queue_conf;
				memset(&queue_conf, 0, sizeof(queue_conf));

				/* schedule type */
				YAML::Node schedule_type_node = y_config_dpdk_ng["dpdk"]["eventdev"]["queues"][queue_id]["schedule_type"];
				if (schedule_type_node && schedule_type_node.IsScalar()) {
					std::string s_schedule_type = schedule_type_node.as<std::string>();
					std::transform(s_schedule_type.begin(), s_schedule_type.end(), s_schedule_type.begin(),
							[](unsigned char c) -> unsigned char { return std::tolower(c); });
					if (s_schedule_type == "ordered") {
						queue_conf.schedule_type = RTE_SCHED_TYPE_ORDERED;
					} else
					if (s_schedule_type == "atomic") {
						queue_conf.schedule_type = RTE_SCHED_TYPE_ATOMIC;
					} else
					if (s_schedule_type == "parallel") {
						queue_conf.schedule_type = RTE_SCHED_TYPE_PARALLEL;
					} else {
						XDPD_ERR(DRIVER_NAME"[processing][init][evdev] eventdev %s, event queue %u, unknown schedule type defined: \"%s\"\n",
								ev_core_tasks[lcore_id].name, queue_id, s_schedule_type.c_str());
						return ROFL_FAILURE;
					}
				} else {
					queue_conf.schedule_type = RTE_SCHED_TYPE_ORDERED;
				}

				/* priority */
				YAML::Node priority_node = y_config_dpdk_ng["dpdk"]["eventdev"]["queues"][queue_id]["priority"];
				if (priority_node && priority_node.IsScalar()) {
					queue_conf.priority = priority_node.as<uint8_t>();
				} else {
					queue_conf.priority = RTE_EVENT_DEV_PRIORITY_NORMAL;
				}

				/* nb_atomic_flows */
				YAML::Node nb_atomic_flows_node = y_config_dpdk_ng["dpdk"]["eventdev"]["queues"][queue_id]["nb_atomic_flows"];
				if (nb_atomic_flows_node && nb_atomic_flows_node.IsScalar()) {
					queue_conf.nb_atomic_flows = nb_atomic_flows_node.as<uint32_t>();
				} else {
					queue_conf.nb_atomic_flows = 1024; /* not used for RTE_SCHED_TYPE_ORDERED */
				}

				/* nb_atomic_order_sequences */
				YAML::Node nb_atomic_order_sequences_node = y_config_dpdk_ng["dpdk"]["eventdev"]["queues"][queue_id]["nb_atomic_order_sequences"];
				if (nb_atomic_order_sequences_node && nb_atomic_order_sequences_node.IsScalar()) {
					queue_conf.nb_atomic_order_sequences = nb_atomic_order_sequences_node.as<uint32_t>();
				} else {
					queue_conf.nb_atomic_order_sequences = ev_core_tasks[lcore_id].eventdev_conf.nb_event_queue_flows;
				}

				XDPD_INFO(DRIVER_NAME"[processing][init][evdev] eventdev %s, ev_queue_id: %2u, schedule-type: %u, priority: %u, nb-atomic-flows: %u, nb-atomic-order-sequences: %u\n",
						ev_core_tasks[lcore_id].name, queue_id, queue_conf.schedule_type, queue_conf.priority, queue_conf.nb_atomic_flows, queue_conf.nb_atomic_order_sequences);
				if (rte_event_queue_setup(ev_core_tasks[lcore_id].eventdev_id, queue_id, &queue_conf) < 0) {
					XDPD_ERR(DRIVER_NAME"[processing][init][evdev] eventdev %s, rte_event_queue_setup() on ev_queue_id: %u failed\n", ev_core_tasks[lcore_id].name, queue_id);
					return ROFL_FAILURE;
				}
			}


			/* map event ports/queues for RX/WK lcores */
			uint8_t ev_port_id = 0;
			uint8_t ev_queue_id = 0;
			{
				/*
				 * configure event port #0 and event queue #0 for control plane to send frames initiated by Packet-Out
				 */
				struct rte_event_port_conf port_conf;
				memset(&port_conf, 0, sizeof(port_conf));
				port_conf.dequeue_depth = ev_core_tasks[lcore_id].eventdev_conf.nb_event_port_dequeue_depth;
				port_conf.enqueue_depth = ev_core_tasks[lcore_id].eventdev_conf.nb_event_port_enqueue_depth;
				port_conf.new_event_threshold = ev_core_tasks[lcore_id].eventdev_conf.nb_events_limit;

				if (rte_event_port_setup(ev_core_tasks[lcore_id].eventdev_id, ev_port_id, &port_conf) < 0) {
					XDPD_ERR(DRIVER_NAME"[processing][init][evdev] eventdev %s, rte_event_port_setup() on ev_port_id: %u failed\n", ev_core_tasks[lcore_id].name, ev_port_id);
					return ROFL_FAILURE;
				}

				/* ev_port_id = 0 assigned to LCORE_ID_ANY */
				XDPD_INFO(DRIVER_NAME"[processing][init][evdev] eventdev %s, LCORE_ID_ANY, ev_queue_id: %2u, ev_port_id: %2u\n",
						ev_core_tasks[lcore_id].name, ev_queue_id, ev_port_id);

				ev_port_id++;
				ev_queue_id++;
			}

			/* assign event ports to RX tasks */
			for (auto rx_lcore_id : rx_lcores[socket_id]) {
				if (not lcores[rx_lcore_id].is_rx_lcore) {
					continue;
				}

				/* testing */
				ev_queue_id = EVENT_QUEUE_TO_WK;

				/* RX core(s) do not receive from an event queue */
				rx_core_tasks[rx_lcore_id].socket_id = socket_id;
				rx_core_tasks[rx_lcore_id].ev_port_id = ev_port_id;
				rx_core_tasks[rx_lcore_id].tx_ev_queue_id = ev_queue_id;

				struct rte_event_port_conf port_conf;
				memset(&port_conf, 0, sizeof(port_conf));
				port_conf.dequeue_depth = ev_core_tasks[lcore_id].eventdev_conf.nb_event_port_dequeue_depth;
				port_conf.enqueue_depth = ev_core_tasks[lcore_id].eventdev_conf.nb_event_port_enqueue_depth;
				port_conf.new_event_threshold = ev_core_tasks[lcore_id].eventdev_conf.nb_events_limit;

				if (rte_event_port_setup(ev_core_tasks[lcore_id].eventdev_id, ev_port_id, &port_conf) < 0) {
					XDPD_ERR(DRIVER_NAME"[processing][init][evdev] eventdev %s, rte_event_port_setup() on ev_port_id: %u failed\n",
							ev_core_tasks[lcore_id].name, ev_port_id);
					return ROFL_FAILURE;
				}

				/* no event queue/port linking for RX cores */
				XDPD_INFO(DRIVER_NAME"[processing][init][evdev] eventdev %s, rx-task-%02u, ev_queue_id: %2u, ev_port_id: %2u\n",
						ev_core_tasks[lcore_id].name, rx_lcore_id, ev_queue_id, ev_port_id);

				ev_port_id++;
				ev_queue_id++;
			}

			/* assign event ports to WK tasks */
			for (auto wk_lcore_id : wk_lcores[socket_id]) {
				if (not lcores[wk_lcore_id].is_wk_lcore) {
					continue;
				}

				/* testing */
				ev_queue_id = EVENT_QUEUE_TO_TX;

				/* worker core(s) read from the associated event queue on their respective NUMA node */
				wk_core_tasks[wk_lcore_id].socket_id = socket_id;
				wk_core_tasks[wk_lcore_id].ev_port_id = ev_port_id;
				wk_core_tasks[wk_lcore_id].tx_ev_queue_id = ev_queue_id;

				struct rte_event_port_conf port_conf;
				memset(&port_conf, 0, sizeof(port_conf));
				port_conf.dequeue_depth = ev_core_tasks[lcore_id].eventdev_conf.nb_event_port_dequeue_depth;
				port_conf.enqueue_depth = ev_core_tasks[lcore_id].eventdev_conf.nb_event_port_enqueue_depth;
				port_conf.new_event_threshold = ev_core_tasks[lcore_id].eventdev_conf.nb_events_limit;

				if (rte_event_port_setup(ev_core_tasks[lcore_id].eventdev_id, ev_port_id, &port_conf) < 0) {
					XDPD_ERR(DRIVER_NAME"[processing][init][evdev] eventdev %s, rte_event_port_setup() on ev_port_id: %u failed\n",
							ev_core_tasks[lcore_id].name, ev_port_id);
					return ROFL_FAILURE;
				}

				/* link up event worker core port and associated queue */
				assert((rx_lcores[socket_id].size()) <= RTE_EVENT_MAX_QUEUES_PER_DEV);

				/* store event queues this worker task is listening to */
				unsigned int index = 0;
				for (auto rx_lcore_id : rx_lcores[socket_id]) {
					wk_core_tasks[wk_lcore_id].rx_ev_queues[index++] = rx_core_tasks[rx_lcore_id].tx_ev_queue_id;
				}
				wk_core_tasks[wk_lcore_id].nb_rx_ev_queues = index;

				/* create info string */
				std::stringstream ss;
				for (unsigned int i = 0; i < wk_core_tasks[wk_lcore_id].nb_rx_ev_queues; i++) {
					ss << (unsigned int)wk_core_tasks[wk_lcore_id].rx_ev_queues[i] << " ";
				}

				XDPD_INFO(DRIVER_NAME"[processing][init][evdev] eventdev %s, wk-task-%02u, ev_queue_id: %2u, ev_port_id: %2u => linked to RX event queues: %s\n",
						ev_core_tasks[lcore_id].name, wk_lcore_id, ev_queue_id, ev_port_id, ss.str().c_str());

				if (rte_event_port_link(ev_core_tasks[lcore_id].eventdev_id, ev_port_id, wk_core_tasks[wk_lcore_id].rx_ev_queues, NULL, wk_core_tasks[wk_lcore_id].nb_rx_ev_queues) < 0) {
					XDPD_ERR(DRIVER_NAME"[processing][init][evdev] eventdev %s, rte_event_port_link() on ev_port_id: %u failed\n",
							ev_core_tasks[lcore_id].name, ev_port_id);
					return ROFL_FAILURE;
				}

				ev_port_id++;
				ev_queue_id++;
			}

			/* assign event ports to TX tasks */
			for (auto tx_lcore_id : tx_lcores[socket_id]) {
				if (not lcores[tx_lcore_id].is_tx_lcore) {
					continue;
				}
				/* TX core(s) read from the associated event queue on their respective NUMA node */
				tx_core_tasks[tx_lcore_id].socket_id = socket_id;
				tx_core_tasks[tx_lcore_id].ev_port_id = ev_port_id;

				struct rte_event_port_conf port_conf;
				memset(&port_conf, 0, sizeof(port_conf));
				port_conf.dequeue_depth = ev_core_tasks[lcore_id].eventdev_conf.nb_event_port_dequeue_depth;
				port_conf.enqueue_depth = ev_core_tasks[lcore_id].eventdev_conf.nb_event_port_enqueue_depth;
				port_conf.new_event_threshold = ev_core_tasks[lcore_id].eventdev_conf.nb_events_limit;

				if (rte_event_port_setup(ev_core_tasks[lcore_id].eventdev_id, ev_port_id, &port_conf) < 0) {
					XDPD_ERR(DRIVER_NAME"[processing][init][evdev] eventdev %s, rte_event_port_setup() on ev_port_id: %u failed\n",
							ev_core_tasks[lcore_id].name, ev_port_id);
					return ROFL_FAILURE;
				}

				/* link up event TX core port and associated queue */
				assert((wk_lcores[socket_id].size() + /*control plane*/1) <= RTE_EVENT_MAX_QUEUES_PER_DEV);

				/* store event queues this worker task is listening to */
				unsigned int index = 0;
				tx_core_tasks[tx_lcore_id].rx_ev_queues[index++] = 0; /* control plane */
				for (auto wk_lcore_id : wk_lcores[socket_id]) {
					tx_core_tasks[tx_lcore_id].rx_ev_queues[index++] = wk_core_tasks[wk_lcore_id].tx_ev_queue_id;
				}
				tx_core_tasks[tx_lcore_id].nb_rx_ev_queues = index;

				/* create info string */
				std::stringstream ss;
				for (unsigned int i = 0; i < tx_core_tasks[tx_lcore_id].nb_rx_ev_queues; i++) {
					ss << (unsigned int)tx_core_tasks[tx_lcore_id].rx_ev_queues[i] << " ";
				}

				XDPD_INFO(DRIVER_NAME"[processing][init][evdev] eventdev %s, tx-task-%02u, ev_port_id: %2u => linked to WK event queues: %s\n",
						ev_core_tasks[lcore_id].name, tx_lcore_id, ev_port_id, ss.str().c_str());

				if (rte_event_port_link(ev_core_tasks[lcore_id].eventdev_id, ev_port_id, tx_core_tasks[tx_lcore_id].rx_ev_queues, NULL, tx_core_tasks[tx_lcore_id].nb_rx_ev_queues) < 0) {
					XDPD_ERR(DRIVER_NAME"[processing][init][evdev] eventdev %s, rte_event_port_link() on ev_port_id: %u failed\n",
							ev_core_tasks[lcore_id].name, ev_port_id);
					return ROFL_FAILURE;
				}

				ev_port_id++;
			}

			/* get event device service_id for service core */
			uint32_t service_id = 0xffffffff;
			if ((ret = rte_event_dev_service_id_get(ev_core_tasks[lcore_id].eventdev_id, &service_id)) < 0) {
				switch (ret) {
				case -ESRCH: {
					/* do nothing: event adapter is not using a service function */
				} break;
				default: {
					/* should never happen */
				};
				}
			} else {
				for (unsigned int lcore_id = 0; lcore_id < rte_lcore_count(); lcore_id++) {
					if (lcore_id >= RTE_MAX_LCORE) {
						continue;
					}
					if (not lcores[lcore_id].is_ev_lcore) {
						continue;
					}
					XDPD_DEBUG(DRIVER_NAME"[processing][init][evdev] mapping service %s (%u) for eventdev %s to service lcore %u\n",
											rte_service_get_name(service_id), service_id, ev_core_tasks[lcore_id].name, lcore_id);
					if ((ret = rte_service_map_lcore_set(service_id, lcore_id, /*enable=*/1)) < 0) {
						XDPD_ERR(DRIVER_NAME"[processing][init][evdev] mapping of service %s (%u) for eventdev %s to service lcore %u failed\n",
								rte_service_get_name(service_id), service_id, ev_core_tasks[lcore_id].name, lcore_id);
						return ROFL_FAILURE;
					}
				}
			}

			/* enable event device service on service lcore */
			if ((ret = rte_service_runstate_set(service_id, 1)) < 0) {
				switch (ret) {
				case -EINVAL: {
					XDPD_ERR(DRIVER_NAME"[processing][init][evdev] service %s (%u) for eventdev %s, setting runstate to true failed (EINVAL)\n",
											rte_service_get_name(service_id), service_id, ev_core_tasks[lcore_id].name);
				} break;
				default: {
					XDPD_ERR(DRIVER_NAME"[processing][init][evdev] service %s (%u) for eventdev %s, setting runstate to true failed\n",
											rte_service_get_name(service_id), service_id, ev_core_tasks[lcore_id].name);
				};
				}
			}

			XDPD_DEBUG(DRIVER_NAME"[processing][init][evdev] service %s (%u) for eventdev %s, runstate: %u\n",
									rte_service_get_name(service_id),
									service_id,
									ev_core_tasks[lcore_id].name,
									rte_service_runstate_get(service_id));
		}
	}
	return ROFL_SUCCESS;
}


/*
* Initialize data structures for processing to work
*/
rofl_result_t processing_init(void){

	//Cleanup
	memset(direct_pools, 0, sizeof(direct_pools));
	memset(indirect_pools, 0, sizeof(indirect_pools));
	memset(rx_core_tasks, 0, sizeof(rx_core_tasks));
	memset(tx_core_tasks, 0, sizeof(tx_core_tasks));
	memset(wk_core_tasks, 0, sizeof(wk_core_tasks));
	memset(ev_core_tasks, 0, sizeof(ev_core_tasks));
	memset(eventdevs, 0, sizeof(eventdevs));
	memset(port_list, 0, sizeof(port_list));

	/*
	 * set log level
	 */
	YAML::Node log_level_node = y_config_dpdk_ng["dpdk"]["eal"]["log_level"];
	if (log_level_node && log_level_node.IsScalar()) {
		rte_log_set_global_level(log_level_node.as<uint32_t>());
		rte_log_set_level(RTE_LOGTYPE_XDPD, log_level_node.as<uint32_t>());
	}

	//Initialize basics
	max_cores = rte_lcore_count();

	rte_rwlock_init(&port_list_rwlock);

	XDPD_DEBUG(DRIVER_NAME"[processing][init] %u logical cores guessed from rte_eal_get_configuration(). Master is: %u\n", rte_lcore_count(), rte_get_master_lcore());

	/*
	 * discover lcores
	 */
	if (ROFL_FAILURE == processing_init_lcores()) {
		XDPD_ERR(DRIVER_NAME"[processing][init] RTE lcore discovery failed\n");
		return ROFL_FAILURE;
	}

	/*
	 * allocate memory
	 */
	if (ROFL_FAILURE == processing_init_task_structures()) {
		XDPD_ERR(DRIVER_NAME"[processing][init] RTE memory allocation failed\n");
		return ROFL_FAILURE;
	}

	/*
	 * initialize RTE event device
	 */
	if (ROFL_FAILURE == processing_init_eventdev()) {
		XDPD_ERR(DRIVER_NAME"[processing][init] RTE event device initialization failed\n");
		return ROFL_FAILURE;
	}

	return ROFL_SUCCESS;
}

/*
* Initialize data structures for processing to work
*/
rofl_result_t processing_run(void){

	int ret;

	/* start service cores */
	for (auto socket_id : numa_nodes) {
		for (auto lcore_id : ev_lcores[socket_id]) {
			XDPD_DEBUG(DRIVER_NAME"[processing][run] starting  service lcore %2u on socket %2u\n", lcore_id, socket_id);
			if ((ret = rte_service_lcore_start(lcore_id)) < 0) {
				switch (ret) {
				case -EALREADY: {
					XDPD_ERR(DRIVER_NAME"[processing][run] start of service lcore %u on socket %u failed (EALREADY)\n", lcore_id, socket_id);
					/* do nothing */
				} break;
				default: {
					XDPD_ERR(DRIVER_NAME"[processing][run] start of service lcore %u on socket %u failed\n", lcore_id, socket_id);
				} return ROFL_FAILURE;
				}
			}
			/* start event device */
			if (rte_event_dev_start(ev_core_tasks[lcore_id].eventdev_id) < 0) {
				XDPD_ERR(DRIVER_NAME"[processing][run] initialization of eventdev %s, rte_event_dev_start() failed\n",
						ev_core_tasks[lcore_id].name);
				return ROFL_FAILURE;
			}
			ev_core_tasks[lcore_id].active = true;
		}
	}

	for (unsigned int lcore_id = 0; lcore_id < rte_lcore_count(); lcore_id++) {
		// sanity check
		if (lcore_id >= RTE_MAX_LCORE) {
			continue;
		}
		// do not start anything on master lcore
		if (lcores[lcore_id].is_master) {
			continue;
		}

		/* event lcores */
		if (lcores[lcore_id].is_ev_lcore) {
			continue;
		}

		/* transmitting lcores */
		if (lcores[lcore_id].is_tx_lcore) {

			// lcore already running?
			if (tx_core_tasks[lcore_id].active == true) {
				continue;
			}

			// lcore should be in state WAIT
			if (rte_eal_get_lcore_state(lcore_id) != WAIT) {
				XDPD_ERR(DRIVER_NAME "[processing][run] ignoring core %u for launching, out of sync (task state != WAIT)\n", lcore_id);
				continue;
			}

			XDPD_DEBUG(DRIVER_NAME "[processing][run] starting transmit lcore %2u on socket %2u\n", lcore_id, tx_core_tasks[lcore_id].socket_id);

			// launch processing task on lcore
			if (rte_eal_remote_launch(&processing_packet_transmission, NULL, lcore_id)) {
				XDPD_ERR(DRIVER_NAME "[processing][run] ignoring lcore %u for starting, as it is not waiting for new task\n", lcore_id);
				continue;
			}

			tx_core_tasks[lcore_id].active = true;
			tx_core_tasks[lcore_id].stats.eths_dropped = 0;
		}

		/* receiving lcores */
		if (lcores[lcore_id].is_rx_lcore) {

			// lcore already running?
			if (rx_core_tasks[lcore_id].active == true) {
				continue;
			}

			// lcore should be in state WAIT
			if (rte_eal_get_lcore_state(lcore_id) != WAIT) {
				XDPD_ERR(DRIVER_NAME "[processing][run] ignoring core %u for launching, out of sync (task state != WAIT)\n", lcore_id);
				continue;
			}

			XDPD_DEBUG(DRIVER_NAME "[processing][run] starting  receive lcore %2u on socket %2u\n", lcore_id, rx_core_tasks[lcore_id].socket_id);

			// launch processing task on lcore
			if (rte_eal_remote_launch(&processing_packet_reception, NULL, lcore_id)) {
				XDPD_ERR(DRIVER_NAME "[processing][run] ignoring lcore %u for starting, as it is not waiting for new task\n", lcore_id);
				continue;
			}

			rx_core_tasks[lcore_id].active = true;
			tx_core_tasks[lcore_id].stats.eths_dropped = 0;
		}

		/* worker lcores */
		if (lcores[lcore_id].is_wk_lcore) {

			// lcore already running?
			if (wk_core_tasks[lcore_id].active == true) {
				continue;
			}

			// lcore should be in state WAIT
			if (rte_eal_get_lcore_state(lcore_id) != WAIT) {
				XDPD_ERR(DRIVER_NAME "[processing][run] ignoring core %u for launching, out of sync (task state != WAIT)\n", lcore_id);
				continue;
			}

			XDPD_DEBUG(DRIVER_NAME "[processing][run] starting   worker lcore %2u on socket %2u\n", lcore_id, wk_core_tasks[lcore_id].socket_id);

			// launch processing task on lcore
			if (rte_eal_remote_launch(&processing_packet_pipeline_processing, NULL, lcore_id)) {
				XDPD_ERR(DRIVER_NAME "[processing][run] ignoring lcore %u for starting, as it is not waiting for new task\n", lcore_id);
				continue;
			}

			wk_core_tasks[lcore_id].active = true;
			wk_core_tasks[lcore_id].stats.eths_dropped = 0;
		}
	}

	//Print the status of the cores
	processing_dump_core_states();

	return ROFL_SUCCESS;
}


/*
* Destroy data structures for processing to work
*/
rofl_result_t processing_shutdown(void){

	int ret;

	XDPD_DEBUG(DRIVER_NAME"[processing][shutdown] Shutting down all active cores\n");

	//Stop all cores and wait for them to complete execution tasks
	for (unsigned int lcore_id = 0; lcore_id < rte_lcore_count(); lcore_id++) {
		if(rx_core_tasks[lcore_id].available && rx_core_tasks[lcore_id].active){
			XDPD_DEBUG(DRIVER_NAME"[processing][shutdown] Shutting down active lcore %u\n", lcore_id);
			rx_core_tasks[lcore_id].active = false;
			//Join core
			rte_eal_wait_lcore(lcore_id);
		}
		if(wk_core_tasks[lcore_id].available && wk_core_tasks[lcore_id].active){
			XDPD_DEBUG(DRIVER_NAME"[processing][shutdown] Shutting down active lcore %u\n", lcore_id);
			wk_core_tasks[lcore_id].active = false;
			//Join core
			rte_eal_wait_lcore(lcore_id);
		}
		if(tx_core_tasks[lcore_id].available && tx_core_tasks[lcore_id].active){
			XDPD_DEBUG(DRIVER_NAME"[processing][shutdown] Shutting down active lcore %u\n", lcore_id);
			tx_core_tasks[lcore_id].active = false;
			//Join core
			rte_eal_wait_lcore(lcore_id);
		}
	}

	/* stop service cores */
	for (auto socket_id : numa_nodes) {
		for (auto lcore_id : ev_lcores[socket_id]) {
			XDPD_DEBUG(DRIVER_NAME"[processing][shutdown] Shutting down event device %s\n", ev_core_tasks[lcore_id].name);
			rte_event_dev_stop(ev_core_tasks[lcore_id].eventdev_id);
			ev_core_tasks[lcore_id].active = false;
			if ((ret = rte_service_lcore_stop(lcore_id)) < 0) {
				switch (ret) {
				case -EALREADY: {
					/* do nothing */
				} break;
				default: {
					XDPD_ERR(DRIVER_NAME"[processing] stop of service lcore %u failed\n", lcore_id);
				};
				}
			}
		}
	}

	return ROFL_SUCCESS;
}

/*
* Destroy data structures for processing to work
*/
rofl_result_t processing_destroy(void){

	for (auto socket_id : numa_nodes) {
		for (auto lcore_id : ev_lcores[socket_id]) {
			/* release event device */
			if (rte_event_dev_close(ev_core_tasks[lcore_id].eventdev_id) < 0) {
				XDPD_ERR(DRIVER_NAME"[processing][shutdown] Unable to stop event device %s\n", ev_core_tasks[lcore_id].name);
				return ROFL_FAILURE;
			}
			ev_core_tasks[lcore_id].eventdev_id = 0;
		}
	}

	return ROFL_SUCCESS;
}



/**
 * RX packet reception
 */
int processing_packet_reception(void* not_used){

	unsigned int i, lcore_id = rte_lcore_id();
	int socket_id = rte_lcore_to_socket_id(lcore_id);
	uint16_t port_id;
	uint16_t queue_id;
	bool up;

#if 0
	switch_port_t* port;
#endif
	rx_core_task_t* task = &rx_core_tasks[lcore_id];
	struct rte_mbuf* mbufs[max_eth_rx_burst_size];
	struct rte_event event[max_eth_rx_burst_size];
	ev_core_task_t* ev_task = eventdevs[socket_id];

	XDPD_INFO(DRIVER_NAME"[processing][tasks][rx] rx-task-%u.%02u: started\n", socket_id, lcore_id);

	if (task->nb_rx_queues == 0) {
		XDPD_INFO(DRIVER_NAME"[processing][tasks][rx] rx-task-%u.%02u: task has no rx-queues assigned, terminating\n", socket_id, lcore_id);
		return ROFL_SUCCESS;
	}

	for (i = 0; i < task->nb_rx_queues; i++) {
		port_id = task->rx_queues[i].port_id;
		queue_id = task->rx_queues[i].queue_id;
		up = task->rx_queues[i].up;
		XDPD_INFO(DRIVER_NAME"[processing][tasks][rx] rx-task-%u.%02u: receiving from port: %u, queue: %u, up: %u\n", socket_id, lcore_id, port_id, queue_id, up);
	}

	//Set flag to active
	task->active = true;

	while(likely(task->active)) {

		for (unsigned int index = 0; index < task->nb_rx_queues; ++index) {

			if (not task->rx_queues[index].up) {
				continue;
			}

			/* read from ethdev queue "queue-id" on port "port-id" */
			port_id = task->rx_queues[index].port_id;
			queue_id = task->rx_queues[index].queue_id;

#if 0
			rte_rwlock_read_lock(&port_list_rwlock);
			if ((port = port_list[port_id]) == NULL) {
				rte_rwlock_read_unlock(&port_list_rwlock);
				continue;
			}

			if (unlikely(not port->up)) { // This CAN happen while deschedulings
				rte_rwlock_read_unlock(&port_list_rwlock);
				continue;
			}
			rte_rwlock_read_unlock(&port_list_rwlock);
#endif
			/* read burst from ethdev */
			const uint16_t nb_rx = rte_eth_rx_burst(port_id, queue_id, mbufs, max_eth_rx_burst_size);

			/* no packets received => continue with next port */
			if (nb_rx==0){
				continue;
			}

			/* map received mbufs to event structure */
			for (i = 0; i < nb_rx; i++) {
				event[i].flow_id = mbufs[i]->hash.rss;
				event[i].op = RTE_EVENT_OP_NEW;
				event[i].sched_type = RTE_SCHED_TYPE_ORDERED;
				//event[i].sched_type = RTE_SCHED_TYPE_PARALLEL;
				//event[i].sched_type = RTE_SCHED_TYPE_ATOMIC;
				if (eventdev_shortcut) {
					event[i].queue_id = tx_core_tasks[*tx_lcores[socket_id].begin()].rx_ev_queues[0];
				} else {
					event[i].queue_id = task->tx_ev_queue_id;
				}
				event[i].event_type = RTE_EVENT_TYPE_ETHDEV;
				event[i].sub_event_type = 0;
				event[i].priority = RTE_EVENT_DEV_PRIORITY_NORMAL;
				event[i].mbuf = mbufs[i];
				mbufs[i]->udata64 = (uint64_t)port_id;

				if (mbufs[i]->port == MBUF_INVALID_PORT) {
					mbufs[i]->port = port_id;
				}
			}
			task->stats.rx_pkts+=nb_rx;

			/* enqueue events to event device */
			const int nb_tx = rte_event_enqueue_burst(ev_task->eventdev_id, task->ev_port_id, event, nb_rx);
			task->stats.tx_evts+=nb_tx;

			RTE_LOG(DEBUG, XDPD, "rx-task-%02u: on socket %u, dequeued %u pkt(s) from eth_port_id=%u and enqueued %u pkt(s) to ev_port_id=%u (rx-eth-burst)\n",
					lcore_id, rte_lcore_to_socket_id(lcore_id), nb_rx, port_id, nb_tx, task->ev_port_id);

			/* release mbufs not queued in event device */
			if (nb_tx < nb_rx) {
				task->stats.evts_dropped+=(nb_rx-nb_tx);
				RTE_LOG(WARNING, XDPD, "rx-task-%02u: dropping %u packets, worker event receive queue full on socket %u, task->stats.evts_dropped=%" PRIu64 "\n",
						lcore_id, nb_rx - nb_tx, rte_lcore_to_socket_id(lcore_id), task->stats.evts_dropped);
				for(i = nb_tx; i < nb_rx; i++) {
					rte_pktmbuf_free(mbufs[i]);
				}
			}

		}
	}

	XDPD_INFO(DRIVER_NAME"[processing][tasks][rx] rx-task-%u.%02u: terminated\n", socket_id, lcore_id);

	return (int)ROFL_SUCCESS;
}


/**
 * Packet pipeline processing
 */
int processing_packet_pipeline_processing(void* not_used){

	unsigned int i, lcore_id = rte_lcore_id();
	int socket_id = rte_lcore_to_socket_id(lcore_id);
	struct rte_event rx_events[max_evt_wk_burst_size];
	wk_core_task_t* task = &wk_core_tasks[lcore_id];
	switch_port_t* port;
	of_switch_t* sw;
	ev_core_task_t* ev_task = eventdevs[socket_id];

	//Parsing and pipeline extra state
	datapacket_t pkt;
	datapacket_dpdk_t* pkt_state = create_datapacket_dpdk(&pkt);

	//Init values and assign
	pkt.platform_state = (platform_datapacket_state_t*)pkt_state;
	pkt_state->mbuf = NULL;

	XDPD_INFO(DRIVER_NAME"[processing][tasks][wk] wk-task-%u.%02u: started\n", socket_id, lcore_id);

	//Set flag to active
	task->active = true;

	while(likely(task->active)) {

		int timeout = 0;
		uint16_t nb_rx = rte_event_dequeue_burst(ev_task->eventdev_id, task->ev_port_id, rx_events, max_evt_wk_burst_size, timeout);

		if (nb_rx == 0) {
			//rte_pause();
			continue;
		}

		RTE_LOG(DEBUG, XDPD, "wk-task-%02u: on socket %u, dequeued %u event(s) from ev_port_id=%u\n",
				lcore_id, rte_lcore_to_socket_id(rte_lcore_id()), nb_rx, task->ev_port_id);

		task->stats.rx_evts+=nb_rx;


		if (pipeline_shortcut){
			dpdk_port_state_t *ps;
			for (i = 0; i < nb_rx; i++) {

				rte_prefetch0(rx_events[i].mbuf);

				rx_events[i].queue_id = task->tx_ev_queue_id;
				uint32_t in_port_id = (uint32_t)(rx_events[i].mbuf->udata64 & 0x00000000ffffffff);
				rte_rwlock_read_lock(&port_list_rwlock);
				if ((port = port_list[in_port_id]) == NULL) {
					rte_rwlock_read_unlock(&port_list_rwlock);
					continue;
				}
				ps = (dpdk_port_state_t *)port->platform_port_state;

				rx_events[i].mbuf->udata64 = (uint64_t)(phyports[ps->port_id].shortcut_port_id);
			}
			rte_event_enqueue_burst(ev_task->eventdev_id, task->ev_port_id, rx_events, nb_rx);
		} else {
			for (i = 0; i < nb_rx; i++) {

				if (rx_events[i].mbuf == NULL) {
					continue;
				}

				rte_prefetch0(rx_events[i].mbuf);

				uint32_t in_port_id = (uint32_t)(rx_events[i].mbuf->udata64 & 0x00000000ffffffff);

				rte_rwlock_read_lock(&port_list_rwlock);
				if ((port = port_list[in_port_id]) == NULL) {
					rte_rwlock_read_unlock(&port_list_rwlock);
					continue;
				}
				sw = port->attached_sw;
				rte_rwlock_read_unlock(&port_list_rwlock);

				/* inject packet into openflow pipeline */
				rx_pkt(lcore_id, sw, rx_events[i].mbuf, &pkt, pkt_state);

				/* see packet_inline.h and src/io/tx.h for transmission of packets */
			}
		}

	}

	destroy_datapacket_dpdk(pkt_state);

	XDPD_INFO(DRIVER_NAME"[processing][tasks][wk] wk-task-%u.%02u: terminated\n", socket_id, lcore_id);

	return (int)ROFL_SUCCESS;
}


/**
 * TX packet transmission
 */
int processing_packet_transmission(void* not_used){

	unsigned int i, lcore_id = rte_lcore_id();
	int socket_id = rte_lcore_to_socket_id(lcore_id);
	tx_core_task_t* task = &tx_core_tasks[lcore_id];
	uint32_t out_port_id;
	struct rte_event tx_events[max_evt_tx_burst_size];
	struct rte_mbuf* tx_pkts[max_eth_tx_burst_size];
	uint64_t cur_tsc;
	ev_core_task_t* ev_task = eventdevs[socket_id];
	unsigned int ret;
	unsigned int nb_elems, nb_elems_remaining;
#if 0
	switch_port_t* port;
#endif

	XDPD_INFO(DRIVER_NAME"[processing][tasks][tx] tx-task-%u.%02u: started\n", socket_id, lcore_id);

	for (unsigned int port_id = 0; port_id < RTE_MAX_ETHPORTS; port_id++) {
		if (not task->tx_queues[port_id].enabled){
			continue;
		}
		uint8_t queue_id = task->tx_queues[port_id].queue_id;
		bool up = task->tx_queues[port_id].up;
		XDPD_INFO(DRIVER_NAME"[processing][tasks][tx] tx-task-%u.%02u: sending via port: %u, queue: %u, up: %u\n", socket_id, lcore_id, port_id, queue_id, up);
	}

	/* initialize port related parameters */
	cur_tsc = rte_get_tsc_cycles();
	for (unsigned int port_id = 0; port_id < RTE_MAX_ETHPORTS; ++port_id){
		/* set txring-last-tx-time for each port to current time */
		task->txring_last_tx_time[port_id] = cur_tsc;
	}

	//Set flag to active
	task->active = true;

	while(likely(task->active)) {

		/*
		 * read events from event queue
		 */
		int timeout = 0;
		uint16_t nb_rx = rte_event_dequeue_burst(ev_task->eventdev_id, task->ev_port_id, tx_events, max_evt_tx_burst_size, timeout);

		task->stats.rx_evts+=nb_rx;

		if (nb_rx>0){
			RTE_LOG(DEBUG, XDPD, "tx-task-%02u: rcvd %u event(s) from ev_port_id %u on eventdev %s with max_evt_tx_burst_size %u\n",
					lcore_id, nb_rx, task->ev_port_id, ev_task->name, max_evt_tx_burst_size);
		}

#if 0
		if (nb_rx==0){
			task->idle_loops++;
			if ((not task->lead_task) && (task->idle_loops > 64)){
				task->active = false;
			}
			continue;
		}
#endif
		if (nb_rx>0){
			task->idle_loops = 0;

			RTE_LOG(DEBUG, XDPD, "tx-task-%02u: on socket %u, dequeued %u event(s) from ev_port_id=%u\n",
					lcore_id, socket_id, nb_rx, task->ev_port_id);

			/* interate over all received events */
			for (i = 0; i < nb_rx; i++) {

				 if (unlikely(tx_events[i].mbuf == NULL)){
					 continue;
				 }

				/* process mbuf using events[i].queue_id as pipeline stage */
				out_port_id = (uint32_t)(tx_events[i].mbuf->udata64 & 0x00000000ffffffff);

#if 0
				rte_rwlock_read_lock(&port_list_rwlock);
				if ((port = port_list[out_port_id]) == NULL) {
					task->stats.bugs_dropped++;
					RTE_LOG(WARNING, XDPD, "tx-task-%02u: outgoing port %u not found, dropping packet, internal error, task->stats.bugs_dropped=%" PRIu64 "\n",
							lcore_id, out_port_id, task->stats.bugs_dropped);
					rte_pktmbuf_free(tx_events[i].mbuf);
					rte_rwlock_read_unlock(&port_list_rwlock);
					continue;
				}
#endif

#if 0
				RTE_LOG(DEBUG, XDPD, "tx-task-%02u: on socket %u received %u events to be sent out on port %u\n",
						lcore_id, socket_id, nb_rx, out_port_id);
#ifdef DEBUG
				{
					dpdk_port_state_t *ps;
					ps = (dpdk_port_state_t *)port->platform_port_state;
					assert(out_port_id == ps->port_id);
				}
#endif

				rte_rwlock_read_unlock(&port_list_rwlock);
#endif

				if (unlikely(task->txring[out_port_id] == NULL)) {
					task->stats.bugs_dropped++;
					RTE_LOG(WARNING, XDPD, "tx-task-%02u: no txring allocated on port %u, dropping packet, internal error, task->stats.bugs_dropped=%" PRIu64 "\n",
							lcore_id, out_port_id, task->stats.bugs_dropped);
					rte_pktmbuf_free(tx_events[i].mbuf);
					continue;
				}

				/* store event.mbuf in txring assigned to outgoing port */
				if ((ret = rte_ring_enqueue(task->txring[out_port_id], tx_events[i].mbuf)) < 0) {
					switch (ret) {
					case -ENOBUFS: {
						task->stats.ring_dropped++;
						RTE_LOG(WARNING, XDPD, "tx-task-%02u: unable to enqueue mbuf from event[%u] to port-id: %u (ENOBUFS), dropping packet, task->stats.ring_dropped=%" PRIu64 "\n",
								lcore_id, i, out_port_id, task->stats.ring_dropped);
						rte_pktmbuf_free(tx_events[i].mbuf);
						continue;
					} break;
					default: {
						task->stats.ring_dropped++;
						RTE_LOG(WARNING, XDPD, "tx-task-%02u: unable to enqueue mbuf from event[%u] to port-id: %u, dropping packet, task->stats.ring_dropped=%" PRIu64 "\n",
								lcore_id, i, out_port_id, task->stats.ring_dropped);
						rte_pktmbuf_free(tx_events[i].mbuf);
						continue;
					};
					}
				}
#if 0
				RTE_LOG(DEBUG, XDPD, "tx-task-%02u: on socket %u, port %u => txring size %u\n",
						lcore_id, socket_id, out_port_id, rte_ring_count(task->txring[out_port_id]));
#endif
			}
		}


		/*
		 * drain all outgoing ports
		 */
		for (unsigned int port_id = 0; port_id < RTE_MAX_ETHPORTS; ++port_id) {

			/* port not enabled in this tx-task */
			if (not task->tx_queues[port_id].enabled || not task->tx_queues[port_id].up) {
				continue;
			}

			/* get number of packets stored in txring */
			if (unlikely((nb_elems=rte_ring_count(task->txring[port_id]))==0)){
				continue;
			}

			cur_tsc = rte_get_tsc_cycles();

			/* if the number of pending packets is lower than txring_drain_threshold or
			 * less time than txring_drain_interval cycles elapsed since
			 * last transmission, skip the port for now and wait for more packets
			 * to arrive in the port's txring queue */
			if (/*(nb_elems < rte_ring_get_capacity(task->txring[port_id])) &&*/
				(nb_elems < task->txring_drain_threshold[port_id]) &&
				(cur_tsc < (task->txring_last_tx_time[port_id] + task->txring_drain_interval[port_id]))) {
				continue;
			}

#ifdef DEBUG
			if (nb_elems >= task->txring_drain_threshold[port_id]){
				RTE_LOG(DEBUG, XDPD, "tx-task-%02u: on socket %u, draining port %u => txring size: %u exceeds txring-drain-threshold: %u, starting tx-eth-burst\n",
						lcore_id, socket_id, port_id, nb_elems, task->txring_drain_threshold[port_id]);
			}
			if (cur_tsc >= (task->txring_last_tx_time[port_id] + task->txring_drain_interval[port_id])){
				RTE_LOG(DEBUG, XDPD, "tx-task-%02u: on socket %u, draining port %u => elapsed time %lfus exceeds txring-drain-interval: %lfms, starting tx-eth-burst\n",
										lcore_id, socket_id, port_id,
										((double)(cur_tsc - task->txring_last_tx_time[port_id]) / rte_get_timer_hz()) * 1e6,
										((double)(task->txring_drain_interval[port_id]) / rte_get_timer_hz()) * 1e6);
			}
#endif

			/* get mbufs from txring */
			nb_elems = rte_ring_dequeue_bulk(task->txring[port_id], (void**)tx_pkts,
								RTE_MIN(nb_elems, (unsigned int)max_eth_tx_burst_size), &nb_elems_remaining);

			/* no elements in txring */
			if (unlikely(nb_elems==0)) {
				continue;
			}

			/* send tx-burst */
			uint16_t nb_tx = rte_eth_tx_burst(port_id, task->tx_queues[port_id].queue_id, tx_pkts, nb_elems);

			task->stats.tx_pkts+=nb_tx;

			/* adjust timestamp */
			task->txring_last_tx_time[port_id] = cur_tsc;

			/* if all packets have been sent, goto next port */
			if (likely(nb_tx==nb_elems)) {
				continue;
			}

			/* otherwise, release any unsent packets */
			task->stats.eths_dropped+=(nb_elems-nb_tx);
			for(i = nb_tx; i < nb_elems; i++) {
				rte_pktmbuf_free(tx_pkts[i]);
			}
			RTE_LOG(WARNING, XDPD, "tx-task-%02u: on socket %u, enqueued %u pkt(s) to eth_port_id=%u (tx-eth-burst), dropping %u pkt(s), remaining txring size: %u, task->stats.eths_dropped=%" PRIu64 "\n",
					lcore_id, socket_id, nb_elems, port_id, nb_elems-nb_tx, rte_ring_count(task->txring[port_id]), task->stats.eths_dropped);
		}
	}

	XDPD_INFO(DRIVER_NAME"[processing][tasks][tx] tx-task-%u.%02u: terminated\n", socket_id, lcore_id);

	return (int)ROFL_SUCCESS;
}



//
//Port scheduling
//

/*
* Schedule port. Schedule port to an available core (RR)
*/
rofl_result_t processing_schedule_port(switch_port_t* port){

	if (!port) {
		return ROFL_SUCCESS;
	}

	dpdk_port_state_t *ps = (dpdk_port_state_t *)port->platform_port_state;

	if (iface_manager_start_port(port) != ROFL_SUCCESS) {
		XDPD_DEBUG(DRIVER_NAME"[processing][port] Starting port %u (%s) failed\n", ps->port_id, port->name);
		assert(0);
		return ROFL_FAILURE;
	}

	if (port->type != PORT_TYPE_PHYSICAL && !ps->port_id) {
		ps->port_id = nb_phy_ports + ((dpdk_kni_port_state_t*)ps)->nf_id;
	}

	{
		rte_rwlock_write_lock(&port_list_rwlock);
		assert(port_list[ps->port_id] == NULL);
		port_list[ps->port_id] = port;
		total_num_of_ports++;
		XDPD_DEBUG(DRIVER_NAME"[processing][port] adding port %u (%s) to active lcores\n", ps->port_id, port->name);
		ps->scheduled = true;
		rte_rwlock_write_unlock(&port_list_rwlock);
	}

	//Print the status of the cores
	processing_dump_core_states();

	return ROFL_SUCCESS;
}

/*
* Deschedule port to a core
*/
rofl_result_t processing_deschedule_port(switch_port_t* port){

	if (!port) {
		return ROFL_SUCCESS;
	}

	dpdk_port_state_t *ps = (dpdk_port_state_t *)port->platform_port_state;

	if (iface_manager_stop_port(port) != ROFL_SUCCESS) {
		XDPD_DEBUG(DRIVER_NAME"[processing][port] Stopping port %u (%s) failed\n", ps->port_id, port->name);
		assert(0);
		return ROFL_FAILURE;
	}

	if (port->type != PORT_TYPE_PHYSICAL && !ps->port_id) {
		ps->port_id = nb_phy_ports + ((dpdk_kni_port_state_t*)ps)->nf_id;
	}

	if (ps->scheduled == false) {
		return ROFL_SUCCESS;
	}

	{
		rte_rwlock_write_lock(&port_list_rwlock);
		assert(port_list[ps->port_id] != NULL);
		port_list[ps->port_id] = NULL;
		total_num_of_ports--;
		XDPD_DEBUG(DRIVER_NAME"[processing][port] dropping port %u (%s) from active lcores\n", ps->port_id, port->name);
		ps->scheduled = false;
		rte_rwlock_write_unlock(&port_list_rwlock);
	}

	//Print the status of the cores
	processing_dump_core_states();

	return ROFL_SUCCESS;
}

/*
* Dump core state
*/
void processing_dump_core_states(void){

	unsigned int i;
	std::stringstream ss;
	enum rte_lcore_role_t role;
	enum rte_lcore_state_t state;

	ss << DRIVER_NAME"[processing] Core status:" << std::endl;

	for(i=0;i<rte_lcore_count();++i){

		//Print basic info
		ss << "\t socket (" << rte_lcore_to_socket_id(i) << ")";

		ss << " core (" << std::setw(3) << i << std::setw(0) << ")";

		//TODO: rwlock (read)
		if(lcores[i].is_master){
			ss << "   master lcore"<<std::endl;
			continue;
		} else
		if (lcores[i].is_ev_lcore){
			ss << "  service lcore";
		} else
		if (lcores[i].is_rx_lcore){
			ss << "  receive lcore";
		} else
		if (lcores[i].is_tx_lcore){
			ss << " transmit lcore";
		} else
		if (lcores[i].is_wk_lcore){
			ss << "   worker lcore";
		}


		role = rte_eal_lcore_role(i);
		state = rte_eal_get_lcore_state(i);

		ss << " role: ";
		switch(role){
			case ROLE_RTE:
				ss << "RTE";
				break;
			case ROLE_OFF:
				ss << "OFF";
				break;
			case ROLE_SERVICE:
				ss << "SVC";
				break;
			default:
				assert(0);
				ss << "Unknown";
				break;
		}

		ss << ", state: ";
		switch(state){
			case WAIT:
				ss << "WAIT";
				break;
			case RUNNING:
				ss << "RUNNING";
				break;
			case FINISHED:
				ss << "FINISHED";
				break;
			default:
				assert(0);
				ss << "UNKNOWN";
				break;
		}

#if 0 // XXX(toanju) reimplement
		ss << " Load factor: "<< std::fixed << std::setprecision(3) << (float)core_task->num_of_rx_ports/PROCESSING_MAX_PORTS_PER_CORE;
		ss << ", serving ports: [";
		for(j=0;j<core_task->num_of_rx_ports;++j){
			if(phy_port_list[j] == NULL){
				ss << "error_NULL,";
				continue;
			}
			ss << phy_port_list[j]->name <<",";
		}
		ss << "]";
#endif
		ss << "\n";
	}

	XDPD_INFO("%s", ss.str().c_str());
}


/**
* Update processing task statistics
*/
void processing_update_stats(void)
{
	XDPD_INFO(DRIVER_NAME"[processing] task status:\n");
	for (auto socket_id : numa_nodes) {
		uint64_t rx_pkts = 0;
		uint64_t tx_pkts = 0;

		for (auto lcore_id : rx_lcores[socket_id]) {
			rx_core_task_t *task = &rx_core_tasks[lcore_id];
			std::stringstream ss;
			ss << "rx-task-" << std::setfill('0') << std::setw(2) << lcore_id << std::setfill(' ') << "(" << task->socket_id  << ")" << ": ";
			ss << "rx-pkts=" << std::setw(16) << task->stats.rx_pkts << ", ";
			ss << "tx-pkts=" << std::setw(16) << task->stats.tx_pkts << ", ";
			ss << "rx-evts=" << std::setw(16) << task->stats.rx_evts << ", ";
			ss << "tx-evts=" << std::setw(16) << task->stats.tx_evts << ", ";
			ss << "evts-dropped=" << std::setw(16) << task->stats.evts_dropped << ", ";
			ss << "bugs-dropped=" << std::setw(16) << task->stats.bugs_dropped << ", ";
			ss << "ring-dropped=" << std::setw(16) << task->stats.ring_dropped << ", ";
			ss << "eths-dropped=" << std::setw(16) << task->stats.eths_dropped << ", ";
			XDPD_INFO(DRIVER_NAME"\t%s\n", ss.str().c_str());
			rx_pkts += task->stats.rx_pkts;
		}
		for (auto lcore_id : wk_lcores[socket_id]) {
			wk_core_task_t *task = &wk_core_tasks[lcore_id];
			std::stringstream ss;
			ss << "wk-task-" << std::setfill('0') << std::setw(2) << lcore_id << std::setfill(' ') << "(" << task->socket_id  << ")" << ": ";
			ss << "rx-pkts=" << std::setw(16) << task->stats.rx_pkts << ", ";
			ss << "tx-pkts=" << std::setw(16) << task->stats.tx_pkts << ", ";
			ss << "rx-evts=" << std::setw(16) << task->stats.rx_evts << ", ";
			ss << "tx-evts=" << std::setw(16) << task->stats.tx_evts << ", ";
			ss << "evts-dropped=" << std::setw(16) << task->stats.evts_dropped << ", ";
			ss << "bugs-dropped=" << std::setw(16) << task->stats.bugs_dropped << ", ";
			ss << "ring-dropped=" << std::setw(16) << task->stats.ring_dropped << ", ";
			ss << "eths-dropped=" << std::setw(16) << task->stats.eths_dropped << ", ";
			XDPD_INFO(DRIVER_NAME"\t%s\n", ss.str().c_str());
		}
		for (auto lcore_id : tx_lcores[socket_id]) {
			tx_core_task_t *task = &tx_core_tasks[lcore_id];
			std::stringstream ss;
			ss << "tx-task-" << std::setfill('0') << std::setw(2) << lcore_id << std::setfill(' ') << "(" << task->socket_id  << ")" << ": ";
			ss << "rx-pkts=" << std::setw(16) << task->stats.rx_pkts << ", ";
			ss << "tx-pkts=" << std::setw(16) << task->stats.tx_pkts << ", ";
			ss << "rx-evts=" << std::setw(16) << task->stats.rx_evts << ", ";
			ss << "tx-evts=" << std::setw(16) << task->stats.tx_evts << ", ";
			ss << "evts-dropped=" << std::setw(16) << task->stats.evts_dropped << ", ";
			ss << "bugs-dropped=" << std::setw(16) << task->stats.bugs_dropped << ", ";
			ss << "ring-dropped=" << std::setw(16) << task->stats.ring_dropped << ", ";
			ss << "eths-dropped=" << std::setw(16) << task->stats.eths_dropped << ", ";
			XDPD_INFO(DRIVER_NAME"\t%s\n", ss.str().c_str());
			tx_pkts += task->stats.tx_pkts;
		}

		std::stringstream ss;
		ss << "Summary socket-" << socket_id << ": ";
		ss << "rx-pkts: " << (unsigned long long)rx_pkts << ", ";
		ss << "tx-pkts: " << (unsigned long long)tx_pkts << ", ";
		ss << "ratio: " << 100*((double)tx_pkts)/((double)rx_pkts) << "% ";
		XDPD_INFO(DRIVER_NAME"\t%s\n", ss.str().c_str());
	}
}


