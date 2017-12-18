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
#include <yaml-cpp/yaml.h>

extern unsigned int mbuf_elems_in_pool;
extern unsigned int mbuf_data_room_size;
extern YAML::Node y_config_dpdk_ng;

using namespace xdpd::gnu_linux_dpdk_ng;

//
// Processing state
//
static unsigned int max_cores;


static unsigned total_num_of_ports = 0;

struct rte_mempool* direct_pools[RTE_MAX_NUMA_NODES];
struct rte_mempool* indirect_pools[RTE_MAX_NUMA_NODES];

switch_port_t* port_list[PROCESSING_MAX_PORTS];
static rte_rwlock_t port_list_rwlock;

/*
 * lcore task structures
 */
/* tasks running on RX lcores */
rx_core_task_t rx_core_tasks[RTE_MAX_LCORE];
/* tasks running on TX lcores */
tx_core_task_t tx_core_tasks[RTE_MAX_LCORE];
/* tasks running on worker lcores */
wk_core_task_t wk_core_tasks[RTE_MAX_LCORE];

/*
 * lcore related parameters
 */
/* a set of available NUMA sockets (socket_id) */
std::set<int> numa_nodes;
/* a map of available event logical cores per NUMA socket (set of lcore_id) */
std::map<unsigned int, std::set<unsigned int> > svc_lcores;
/* a map of available event logical cores per NUMA socket (set of lcore_id) */
std::map<unsigned int, std::set<unsigned int> > ev_lcores;
/* a map of available RX logical cores per NUMA socket (set of lcore_id) */
std::map<unsigned int, std::set<unsigned int> > rx_lcores;
/* a map of available TX logical cores per NUMA socket (set of lcore_id) */
std::map<unsigned int, std::set<unsigned int> > tx_lcores;
/* a map of available worker logical cores per NUMA socket (set of lcore_id) */
std::map<unsigned int, std::set<unsigned int> > wk_lcores;

/* service lcores */
static uint64_t svc_coremask = 0x0001; // lcore_id = 0
/* RX lcores */
static uint64_t rx_coremask  = 0x0002; // lcore_id = 1
/* TX lcores */
static uint64_t tx_coremask  = 0x0004; // lcore_id = 2
/* WK lcores */
static uint64_t wk_coremask  = 0x0008; // lcore_id = 4

/*
 * eventdev related parameters
 */
/* event device name */
std::string eventdev_name("event_sw0");
/* event device arguments */
std::string eventdev_args("sched_quanta=64,credit_quanta=32");
/* event device handle */
uint8_t eventdev_id = 0;
/* event device info structure */
struct rte_event_dev_info eventdev_info;
/* event device configuration */
struct rte_event_dev_config eventdev_conf;
/* maximum number of event queues per NUMA node: queue[0]=used by workers, queue[1]=used by TX lcores */
enum event_queue_t {
	EVENT_QUEUE_WORKERS = 0,
	EVENT_QUEUE_TXCORES = 1,
	EVENT_QUEUE_MAX = 2, /* max number of event queues per NUMA node */
};
/* event queues on all NUMA nodes */
uint8_t event_queues[RTE_MAX_NUMA_NODES][EVENT_QUEUE_MAX];


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
	}
	numa_nodes.clear();
	wk_lcores.clear();

	//Get master lcore
	unsigned int master_lcore_id = rte_get_master_lcore();

	/* get svc coremask */
	YAML::Node svc_coremask_node = y_config_dpdk_ng["dpdk"]["lcores"]["svc_coremask"];
	if (svc_coremask_node && svc_coremask_node.IsScalar()) {
		svc_coremask = svc_coremask_node.as<uint64_t>();
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
			//service lcore (=service cores)
			if (svc_coremask & ((uint64_t)1 << lcore_id)) {
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
				lcores[lcore_id].is_svc_lcore = 1;
				//Increase number of service lcores for this socket
				svc_lcores[socket_id].insert(lcore_id);
				s_task.assign("service lcore");
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

/*
* Initialize data structures for RTE event device
*/
rofl_result_t processing_init_eventdev(void){

	int ret;

	/*
	 * initialize eventdev device
	 */
	XDPD_DEBUG(DRIVER_NAME"[processing][init][evdev] initializing eventdev device\n");

	/* get software event name */
	YAML::Node eventdev_name_node = y_config_dpdk_ng["dpdk"]["eventdev"]["name"];
	if (eventdev_name_node && eventdev_name_node.IsScalar()) {
		eventdev_name = eventdev_name_node.as<std::string>();
	}

	/* get software event arguments */
	YAML::Node eventdev_args_node = y_config_dpdk_ng["dpdk"]["eventdev"]["args"];
	if (eventdev_args_node && eventdev_args_node.IsScalar()) {
		eventdev_args = eventdev_args_node.as<std::string>();
	}

	/* initialize software event pmd */
	if ((ret = rte_vdev_init(eventdev_name.c_str(), eventdev_args.c_str())) < 0) {
		switch (ret) {
		case -EINVAL: {
			XDPD_ERR(DRIVER_NAME"[processing][init][evdev] initialization of eventdev %s with args \"%s\" failed (EINVAL)\n", eventdev_name.c_str(), eventdev_args.c_str());
		} break;
		case -EEXIST: {
			XDPD_ERR(DRIVER_NAME"[processing][init][evdev] initialization of eventdev %s with args \"%s\" failed (EEXIST)\n", eventdev_name.c_str(), eventdev_args.c_str());
		} break;
		case -ENOMEM: {
			XDPD_ERR(DRIVER_NAME"[processing][init][evdev] initialization of eventdev %s with args \"%s\" failed (ENOMEM)\n", eventdev_name.c_str(), eventdev_args.c_str());
		} break;
		default: {
			XDPD_ERR(DRIVER_NAME"[processing][init][evdev] initialization of eventdev %s with args \"%s\" failed\n", eventdev_name.c_str(), eventdev_args.c_str());
		};
		}
		return ROFL_FAILURE;
	}
	uint8_t nb_event_devs = rte_event_dev_count();
	XDPD_DEBUG(DRIVER_NAME"[processing][init][evdev] %u eventdev device(s) available\n", nb_event_devs);

	/* get eventdev id */
	eventdev_id = rte_event_dev_get_dev_id(eventdev_name.c_str());

	/* get eventdev info structure */
	if ((ret = rte_event_dev_info_get(eventdev_id, &eventdev_info)) < 0) {
		XDPD_ERR(DRIVER_NAME"[processing][init][evdev] unable to retrieve info struct for eventdev %s\n", eventdev_name.c_str());
	}

	XDPD_DEBUG(DRIVER_NAME"[processing][init][evdev] eventdev: %s, max_event_ports: %u, max_event_queues: %u\n",
			eventdev_name.c_str(), eventdev_info.max_event_ports, eventdev_info.max_event_queues);


	/* configure event device */
	memset(&eventdev_conf, 0, sizeof(eventdev_conf));
	eventdev_conf.nb_event_queues = 2 * numa_nodes.size(); /* RX(s) =(single queue)=> workers =(single queue)=> TX(s) : 2 queues per NUMA node */
	eventdev_conf.nb_event_ports = 0;
	unsigned int nb_wk_lcores = 0;
	unsigned int nb_tx_lcores = 0;
	unsigned int nb_rx_lcores = 0;
	for (auto it : rx_lcores) {
		eventdev_conf.nb_event_ports += it.second.size(); /* number of all RX lcores on all NUMA sockets */
		nb_rx_lcores += it.second.size();
	}
	for (auto it : tx_lcores) {
		eventdev_conf.nb_event_ports += it.second.size(); /* number of all TX lcores on all NUMA sockets */
		nb_tx_lcores += it.second.size();
	}
	for (auto it : wk_lcores) {
		eventdev_conf.nb_event_ports += it.second.size(); /* number of all worker lcores on all NUMA sockets */
		nb_wk_lcores += it.second.size();
	}
	if (eventdev_conf.nb_event_ports > eventdev_info.max_event_ports) {
		XDPD_ERR(DRIVER_NAME"[processing][init][evdev] initialization of eventdev %s failed, too many event ports required\n", eventdev_name.c_str());
		return ROFL_FAILURE;
	}
	eventdev_conf.nb_events_limit = eventdev_info.max_num_events;
	eventdev_conf.nb_event_queue_flows = eventdev_info.max_event_queue_flows;
	eventdev_conf.nb_event_port_dequeue_depth = eventdev_info.max_event_port_dequeue_depth;
	eventdev_conf.nb_event_port_enqueue_depth = eventdev_info.max_event_port_enqueue_depth;

	XDPD_DEBUG(DRIVER_NAME"[processing][init][evdev] configuring eventdev: %s, nb_event_queues: %u, nb_event_ports: %u, nb_events_limit: %u, nb_event_queue_flows: %u, nb_event_port_dequeue_depth: %u, nb_event_port_enqueue_depth: %u\n",
			eventdev_name.c_str(), eventdev_conf.nb_event_queues, eventdev_conf.nb_event_ports,
			eventdev_conf.nb_events_limit, eventdev_conf.nb_event_queue_flows,
			eventdev_conf.nb_event_port_dequeue_depth, eventdev_conf.nb_event_port_enqueue_depth);

	if ((ret = rte_event_dev_configure(eventdev_id, &eventdev_conf)) < 0) {
		XDPD_ERR(DRIVER_NAME"[processing][init][evdev] eventdev %s, rte_event_dev_configure() failed\n", eventdev_name.c_str());
		return ROFL_FAILURE;
	}


	/* configure event queues */
	for (unsigned int queue_id = 0; queue_id < eventdev_conf.nb_event_queues; queue_id++) {
		struct rte_event_queue_conf queue_conf;
		memset(&queue_conf, 0, sizeof(queue_conf));

		/* schedule type */
		YAML::Node schedule_type_node = y_config_dpdk_ng["dpdk"]["eventdev"]["queues"][queue_id]["schedule_type"];
		if (schedule_type_node && schedule_type_node.IsScalar()) {
			queue_conf.schedule_type = schedule_type_node.as<uint8_t>();
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
			queue_conf.nb_atomic_flows = nb_atomic_flows_node.as<uint8_t>();
		} else {
			queue_conf.nb_atomic_flows = 1024; /* not used for RTE_SCHED_TYPE_ORDERED */
		}

		/* nb_atomic_order_sequences */
		YAML::Node nb_atomic_order_sequences_node = y_config_dpdk_ng["dpdk"]["eventdev"]["queues"][queue_id]["nb_atomic_order_sequences_node"];
		if (nb_atomic_order_sequences_node && nb_atomic_order_sequences_node.IsScalar()) {
			queue_conf.nb_atomic_order_sequences = nb_atomic_order_sequences_node.as<uint8_t>();
		} else {
			queue_conf.nb_atomic_order_sequences = eventdev_conf.nb_event_queue_flows;
		}

		XDPD_INFO(DRIVER_NAME"[processing][init][evdev] eventdev %s, queue_id: %2u, schedule-type: %u, priority: %u, nb-atomic-flows: %u, nb-atomic-order-sequences: %u\n",
				eventdev_name.c_str(), queue_id, queue_conf.schedule_type, queue_conf.priority, queue_conf.nb_atomic_flows, queue_conf.nb_atomic_order_sequences);
		if (rte_event_queue_setup(eventdev_id, queue_id, &queue_conf) < 0) {
			XDPD_ERR(DRIVER_NAME"[processing][init][evdev] eventdev %s, rte_event_queue_setup() on queue_id: %u failed\n", eventdev_name.c_str(), queue_id);
			return ROFL_FAILURE;
		}
	}


	/* map event queues for TX/worker lcores on active NUMA nodes */
	uint8_t queue_id = 0;
	for (auto socket_id : numa_nodes) {

		event_queues[socket_id][EVENT_QUEUE_WORKERS] = queue_id++;
		event_queues[socket_id][EVENT_QUEUE_TXCORES] = queue_id++;

		if (queue_id > eventdev_conf.nb_event_queues) {
			XDPD_ERR(DRIVER_NAME"[processing][init][evdev] eventdev %s, internal error, queue_id %u not valid\n", eventdev_name.c_str(), queue_id);
			return ROFL_FAILURE;
		}
	}


	/* map event ports for TX/worker lcores on active NUMA nodes */
	uint8_t port_id = 0;
	for (unsigned int lcore_id = 0; lcore_id < rte_lcore_count(); lcore_id++) {
		if (port_id > eventdev_conf.nb_event_ports) {
			XDPD_ERR(DRIVER_NAME"[processing][init][evdev] eventdev %s, internal error, port_id %u not valid\n", eventdev_name.c_str(), port_id);
			break;
		}
		if (lcore_id >= RTE_MAX_LCORE) {
			continue;
		}
		unsigned int socket_id = rte_lcore_to_socket_id(lcore_id);

		if (lcores[lcore_id].is_master) {
			/* master core */
			continue;
		} else
		if (lcores[lcore_id].is_svc_lcore) {
			/* service core(s) */
			continue;
		} else
		if (lcores[lcore_id].is_rx_lcore) {
			/* RX core(s) do not receive from an event queue */
			rx_core_tasks[lcore_id].socket_id = socket_id;
			rx_core_tasks[lcore_id].ev_port_id = port_id;
			rx_core_tasks[lcore_id].tx_ev_queue_id = event_queues[socket_id][EVENT_QUEUE_WORKERS];

			struct rte_event_port_conf port_conf;
			memset(&port_conf, 0, sizeof(port_conf));
			port_conf.dequeue_depth = eventdev_conf.nb_event_port_dequeue_depth;
			port_conf.enqueue_depth = eventdev_conf.nb_event_port_enqueue_depth;
			port_conf.new_event_threshold = eventdev_conf.nb_events_limit;

			if (rte_event_port_setup(eventdev_id, port_id, &port_conf) < 0) {
				XDPD_ERR(DRIVER_NAME"[processing][init][evdev] eventdev %s, rte_event_port_setup() on port_id: %u failed\n", eventdev_name.c_str(), port_id);
				return ROFL_FAILURE;
			}

			/* no event queue/port linking for RX cores */
			XDPD_DEBUG(DRIVER_NAME"[processing][init][evdev] eventdev %s,  port_id: %2u, rx lcore %u\n", eventdev_name.c_str(), port_id, lcore_id);

		} else
		if (lcores[lcore_id].is_tx_lcore) {
			/* TX core(s) read from the associated event queue on their respective NUMA node */
			tx_core_tasks[lcore_id].socket_id = socket_id;
			tx_core_tasks[lcore_id].ev_port_id = port_id;
			tx_core_tasks[lcore_id].rx_ev_queue_id = event_queues[socket_id][EVENT_QUEUE_TXCORES];

			struct rte_event_port_conf port_conf;
			memset(&port_conf, 0, sizeof(port_conf));
			port_conf.dequeue_depth = eventdev_conf.nb_event_port_dequeue_depth;
			port_conf.enqueue_depth = eventdev_conf.nb_event_port_enqueue_depth;
			port_conf.new_event_threshold = eventdev_conf.nb_events_limit;

			if (rte_event_port_setup(eventdev_id, port_id, &port_conf) < 0) {
				XDPD_ERR(DRIVER_NAME"[processing][init][evdev] eventdev %s, rte_event_port_setup() on port_id: %u failed\n", eventdev_name.c_str(), port_id);
				return ROFL_FAILURE;
			}

			/* link up event TX core port and associated queue */
			XDPD_DEBUG(DRIVER_NAME"[processing][init][evdev] eventdev %s,  port_id: %2u, tx lcore %u, linked to queue_id: %u\n",
					eventdev_name.c_str(), port_id, lcore_id, tx_core_tasks[lcore_id].rx_ev_queue_id);

			uint8_t queues[] = { tx_core_tasks[lcore_id].rx_ev_queue_id };

			if (rte_event_port_link(eventdev_id, port_id, queues, NULL, sizeof(queues)) < 0) {
				XDPD_ERR(DRIVER_NAME"[processing][init][evdev] eventdev %s, rte_event_port_link() on port_id: %u failed\n", eventdev_name.c_str(), port_id);
				return ROFL_FAILURE;
			}

		} else
		if (lcores[lcore_id].is_wk_lcore) {
			/* worker core(s) read from the associated event queue on their respective NUMA node */
			wk_core_tasks[lcore_id].socket_id = socket_id;
			wk_core_tasks[lcore_id].ev_port_id = port_id;
			wk_core_tasks[lcore_id].rx_ev_queue_id = event_queues[socket_id][EVENT_QUEUE_WORKERS];
			for (auto i : numa_nodes) {
				wk_core_tasks[lcore_id].tx_ev_queue_id[i] = event_queues[i][EVENT_QUEUE_TXCORES];
			}

			struct rte_event_port_conf port_conf;
			memset(&port_conf, 0, sizeof(port_conf));
			port_conf.dequeue_depth = eventdev_conf.nb_event_port_dequeue_depth;
			port_conf.enqueue_depth = eventdev_conf.nb_event_port_enqueue_depth;
			port_conf.new_event_threshold = eventdev_conf.nb_events_limit;

			if (rte_event_port_setup(eventdev_id, port_id, &port_conf) < 0) {
				XDPD_ERR(DRIVER_NAME"[processing][init][evdev] eventdev %s, rte_event_port_setup() on port_id: %u failed\n", eventdev_name.c_str(), port_id);
				return ROFL_FAILURE;
			}

			/* link up event worker core port and associated queue */
			XDPD_DEBUG(DRIVER_NAME"[processing][init][evdev] eventdev %s,  port_id: %2u, wk lcore %u, linked to queue_id: %u\n",
					eventdev_name.c_str(), port_id, lcore_id, wk_core_tasks[lcore_id].rx_ev_queue_id);

			uint8_t queues[] = { wk_core_tasks[lcore_id].rx_ev_queue_id };

			if (rte_event_port_link(eventdev_id, port_id, queues, NULL, sizeof(queues)) < 0) {
				XDPD_ERR(DRIVER_NAME"[processing][init][evdev] eventdev %s, rte_event_port_link() on port_id: %u failed\n", eventdev_name.c_str(), port_id);
				return ROFL_FAILURE;
			}
		}
		port_id++;
	}



	/* get event device service_id for service core */
	uint32_t service_id = 0xffffffff;
	if ((ret = rte_event_dev_service_id_get(eventdev_id, &service_id)) < 0) {
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
			if (not lcores[lcore_id].is_svc_lcore) {
				continue;
			}
			XDPD_DEBUG(DRIVER_NAME"[processing][init][evdev] mapping service %s (%u) for eventdev %s to service lcore %u\n",
									rte_service_get_name(service_id), service_id, eventdev_name.c_str(), lcore_id);
			if ((ret = rte_service_map_lcore_set(service_id, lcore_id, /*enable=*/1)) < 0) {
				XDPD_ERR(DRIVER_NAME"[processing][init][evdev] mapping of service %s (%u) for eventdev %s to service lcore %u failed\n",
						rte_service_get_name(service_id), service_id, eventdev_name.c_str(), lcore_id);
				return ROFL_FAILURE;
			}
		}
	}

	/* enable event device service on service lcore */
	if ((ret = rte_service_runstate_set(service_id, 1)) < 0) {
		switch (ret) {
		case -EINVAL: {
			XDPD_ERR(DRIVER_NAME"[processing][init][evdev] service %s (%u) for eventdev %s, setting runstate to true failed (EINVAL)\n",
									rte_service_get_name(service_id), service_id, eventdev_name.c_str());
		} break;
		default: {
			XDPD_ERR(DRIVER_NAME"[processing][init][evdev] service %s (%u) for eventdev %s, setting runstate to true failed\n",
									rte_service_get_name(service_id), service_id, eventdev_name.c_str());
		};
		}
	}

	XDPD_DEBUG(DRIVER_NAME"[processing][init][evdev] service %s (%u) for eventdev %s, runstate: %u\n",
							rte_service_get_name(service_id), service_id, eventdev_name.c_str(), rte_service_runstate_get(service_id));

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
	memset(port_list, 0, sizeof(port_list));

	/*
	 * set log level
	 */
	YAML::Node log_level_node = y_config_dpdk_ng["dpdk"]["log_level"];
	if (log_level_node && log_level_node.IsScalar()) {
		rte_log_set_global_level(log_level_node.as<uint32_t>());
		rte_log_set_level(RTE_LOGTYPE_USER1, log_level_node.as<uint32_t>());
	}

	/*
	 * discover lcores
	 */
	if (ROFL_FAILURE == processing_init_lcores()) {
		XDPD_ERR(DRIVER_NAME"[processing][init] RTE lcore discovery failed\n");
		return ROFL_FAILURE;
	}

	/*
	 * initialize RTE event device
	 */
	if (ROFL_FAILURE == processing_init_eventdev()) {
		XDPD_ERR(DRIVER_NAME"[processing][init] RTE event device initialization failed\n");
		return ROFL_FAILURE;
	}

	//Initialize basics
	max_cores = rte_lcore_count();

	rte_rwlock_init(&port_list_rwlock);

	XDPD_DEBUG(DRIVER_NAME"[processing][init] %u logical cores guessed from rte_eal_get_configuration(). Master is: %u\n", rte_lcore_count(), rte_get_master_lcore());
	//mp_hdlr_init_ops_mp_mc();


	YAML::Node mbuf_elems_node = y_config_dpdk_ng["dpdk"]["mbuf_elems_in_pool"];
	if (mbuf_elems_node && mbuf_elems_node.IsScalar()) {
		mbuf_elems_in_pool = mbuf_elems_node.as<unsigned int>();
	}

	YAML::Node mbuf_data_node = y_config_dpdk_ng["dpdk"]["mbuf_data_room_size"];
	if (mbuf_data_node && mbuf_data_node.IsScalar()) {
		mbuf_data_room_size = mbuf_data_node.as<unsigned int>();
	}

	//Define available cores
	for (unsigned int lcore_id = 0; lcore_id < rte_lcore_count(); lcore_id++) {
		enum rte_lcore_role_t role = rte_eal_lcore_role(lcore_id);
		if(role == ROLE_RTE){

			if (lcores[lcore_id].is_master){
				continue;
			}
			if (lcores[lcore_id].is_svc_lcore) {
				continue;
			}
			if (lcores[lcore_id].is_wk_lcore) {
				wk_core_tasks[lcore_id].available = true;
				continue;
			}

			//XDPD_DEBUG(DRIVER_NAME"[processing][init] marking core %u as available\n", lcore_id);

			//Recover CPU socket for the lcore
			unsigned int socket_id = rte_lcore_to_socket_id(lcore_id);

			/*
			 * Initialize memory for NUMA socket (socket_id)
			 */

			/* direct mbufs */
			if(direct_pools[socket_id] == NULL){

				/**
				*  create the mbuf pool for that socket id
				*/
				char pool_name[RTE_MEMPOOL_NAMESIZE];
				snprintf (pool_name, RTE_MEMPOOL_NAMESIZE, "pool_direct_%u", socket_id);
				XDPD_INFO(DRIVER_NAME"[processing][init] creating mempool %s with %u mbufs each of size %u bytes for CPU socket %u\n", pool_name, mbuf_elems_in_pool, mbuf_data_room_size, socket_id);

				direct_pools[socket_id] = rte_pktmbuf_pool_create(
						pool_name,
						/*number of elements in pool=*/mbuf_elems_in_pool,
						/*cache_size=*/0,
						/*priv_size=*/RTE_ALIGN(sizeof(struct rte_pktmbuf_pool_private), RTE_MBUF_PRIV_ALIGN),
						/*data_room_size=*/mbuf_data_room_size,
						socket_id);

				if (direct_pools[socket_id] == NULL) {
					XDPD_INFO(DRIVER_NAME"[processing][init] unable to allocate mempool %s due to error %u (%s)\n", pool_name, rte_errno, rte_strerror(rte_errno));
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
				XDPD_INFO(DRIVER_NAME"[processing][init] creating mempool %s with %u mbufs each of size %u bytes for CPU socket %u\n", pool_name, mbuf_elems_in_pool, mbuf_data_room_size, socket_id);

				indirect_pools[socket_id] = rte_pktmbuf_pool_create(
						pool_name,
						/*number of elements in pool=*/mbuf_elems_in_pool,
						/*cache_size=*/0,
						/*priv_size=*/RTE_ALIGN(sizeof(struct rte_pktmbuf_pool_private), RTE_MBUF_PRIV_ALIGN),
						/*data_room_size=*/mbuf_data_room_size,
						socket_id);

				if (indirect_pools[socket_id] == NULL) {
					XDPD_INFO(DRIVER_NAME"[processing][init] unable to allocate mempool %s due to error %u (%s)\n", pool_name, rte_errno, rte_strerror(rte_errno));
					rte_panic("Cannot initialize indirect mbuf pool for CPU socket: %u\n", socket_id);
				}
			}
		}
	}
	return ROFL_SUCCESS;
}

/*
* Initialize data structures for processing to work
*/
rofl_result_t processing_run(void){

	int ret;

	/* start service cores */
	for (unsigned int lcore_id = 0; lcore_id < rte_lcore_count(); lcore_id++) {
		if (not lcores[lcore_id].is_svc_lcore) {
			continue;
		}
		XDPD_DEBUG(DRIVER_NAME"[processing][run] starting service lcore %u\n", lcore_id);
		if ((ret = rte_service_lcore_start(lcore_id)) < 0) {
			switch (ret) {
			case -EALREADY: {
				XDPD_ERR(DRIVER_NAME"[processing][run] start of service lcore %u failed (EALREADY)\n", lcore_id);
				/* do nothing */
			} break;
			default: {
				XDPD_ERR(DRIVER_NAME"[processing][run] start of service lcore %u failed\n", lcore_id);
			} return ROFL_FAILURE;
			}
		}
	}

	/* start event device */
	if (rte_event_dev_start(eventdev_id) < 0) {
		XDPD_ERR(DRIVER_NAME"[processing][run] initialization of eventdev %s, rte_event_dev_start() failed\n", eventdev_name.c_str());
		return ROFL_FAILURE;
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

		/* transmitting lcores */
		if (lcores[lcore_id].is_tx_lcore) {

			// lcore already running?
			if (wk_core_tasks[lcore_id].active == true) {
				continue;
			}

			// lcore should be in state WAIT
			if (rte_eal_get_lcore_state(lcore_id) != WAIT) {
				XDPD_ERR(DRIVER_NAME "[processing][run] ignoring core %u for launching, out of sync (task state != WAIT)\n", lcore_id);
				continue;
			}

			XDPD_DEBUG(DRIVER_NAME "[processing][run] starting TX lcore %u on socket (%u)\n", lcore_id, tx_core_tasks[lcore_id].socket_id);

			// launch processing task on lcore
			if (rte_eal_remote_launch(&processing_packet_transmission, NULL, lcore_id)) {
				XDPD_ERR(DRIVER_NAME "[processing][run] ignoring lcore %u for starting, as it is not waiting for new task\n", lcore_id);
				continue;
			}

			wk_core_tasks[lcore_id].active = true;
		}

		/* receiving lcores */
		if (lcores[lcore_id].is_rx_lcore) {

			// lcore already running?
			if (wk_core_tasks[lcore_id].active == true) {
				continue;
			}

			// lcore should be in state WAIT
			if (rte_eal_get_lcore_state(lcore_id) != WAIT) {
				XDPD_ERR(DRIVER_NAME "[processing][run] ignoring core %u for launching, out of sync (task state != WAIT)\n", lcore_id);
				continue;
			}

			XDPD_DEBUG(DRIVER_NAME "[processing][run] starting RX lcore %u on socket %u\n", lcore_id, rx_core_tasks[lcore_id].socket_id);

			// launch processing task on lcore
			if (rte_eal_remote_launch(&processing_packet_reception, NULL, lcore_id)) {
				XDPD_ERR(DRIVER_NAME "[processing][run] ignoring lcore %u for starting, as it is not waiting for new task\n", lcore_id);
				continue;
			}

			wk_core_tasks[lcore_id].active = true;
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

			XDPD_DEBUG(DRIVER_NAME "[processing][run] starting worker lcore %u on socket %u\n", lcore_id, wk_core_tasks[lcore_id].socket_id);

			// launch processing task on lcore
			if (rte_eal_remote_launch(&processing_core_process_packets, NULL, lcore_id)) {
				XDPD_ERR(DRIVER_NAME "[processing][run] ignoring lcore %u for starting, as it is not waiting for new task\n", lcore_id);
				continue;
			}

			wk_core_tasks[lcore_id].active = true;
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
		if(wk_core_tasks[lcore_id].available && wk_core_tasks[lcore_id].active){
			XDPD_DEBUG(DRIVER_NAME"[processing][shutdown] Shutting down active lcore %u\n", lcore_id);
			wk_core_tasks[lcore_id].active = false;
			//Join core
			rte_eal_wait_lcore(lcore_id);
		}
	}

	XDPD_DEBUG(DRIVER_NAME"[processing][shutdown] Shutting down event device %s\n", eventdev_name.c_str());
	rte_event_dev_stop(eventdev_id);

	/* stop service cores */
	for (unsigned int lcore_id = 0; lcore_id < rte_lcore_count(); lcore_id++) {
		if (not lcores[lcore_id].is_svc_lcore) {
			continue;
		}
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

	return ROFL_SUCCESS;
}

/*
* Destroy data structures for processing to work
*/
rofl_result_t processing_destroy(void){

	/* release event device */
	if (rte_event_dev_close(eventdev_id) < 0) {
		XDPD_ERR(DRIVER_NAME"[processing][shutdown] Unable to stop event device %s\n", eventdev_name.c_str());
		return ROFL_FAILURE;
	}
	eventdev_id = 0;

	return ROFL_SUCCESS;
}



/**
 * RX packet reception
 */
int processing_packet_reception(void* not_used){

	unsigned int i, lcore_id = rte_lcore_id();
	uint16_t port_id;
	uint16_t queue_id;
	uint8_t ev_port_id;
	uint8_t ev_queue_id;
	switch_port_t* port;
	rx_core_task_t* task = &rx_core_tasks[lcore_id];

	if (task->nb_rx_queues == 0) {
		RTE_LOG(INFO, USER1, "lcore %u has no rx-queues assigned, terminating\n", lcore_id);
		return ROFL_SUCCESS;
	}

	//Set flag to active
	task->active = true;

	for (i = 0; i < task->nb_rx_queues; i++) {
		port_id = task->rx_queues[i].port_id;
		queue_id = task->rx_queues[i].queue_id;
		RTE_LOG(INFO, USER1, " -- RX lcore_id=%u port_id=%hhu rx_queue_id=%hhu\n", lcore_id, port_id, queue_id);
	}

	RTE_LOG(INFO, USER1, "run RX task on lcore_id %u\n", lcore_id);

	while(likely(task->active)) {

		for (unsigned int index = 0; index < task->nb_rx_queues; ++index) {

			/* read from ethdev queue "queue-id" on port "port-id" */
			port_id = task->rx_queues[index].port_id;
			queue_id = task->rx_queues[index].queue_id;
			/* write event to evdev queue "ev-queue-id" via port "ev-port-id" */
			ev_port_id = task->ev_port_id;
			ev_queue_id = task->tx_ev_queue_id;

			rte_rwlock_read_lock(&port_list_rwlock);
			if ((port = port_list[port_id]) == NULL) {
				rte_rwlock_read_unlock(&port_list_rwlock);
				continue;
			}

			if (likely(port->up)) { // This CAN happen while deschedulings

				struct rte_mbuf* mbufs[PROC_ETH_RX_BURST_SIZE];
				struct rte_event event[PROC_ETH_RX_BURST_SIZE];

				const uint16_t nb_rx = rte_eth_rx_burst(port_id, queue_id, mbufs, PROC_ETH_RX_BURST_SIZE);

				if (nb_rx) {
					RTE_LOG(INFO, USER1, "RX task %u => %u packets received from eth-port %u, eth-queue %u\n", lcore_id, nb_rx, port_id, queue_id);

					for (i = 0; i < nb_rx; i++) {
						event[i].flow_id = mbufs[i]->hash.rss;
						event[i].op = RTE_EVENT_OP_NEW;
						event[i].sched_type = RTE_SCHED_TYPE_ATOMIC;
						event[i].queue_id = ev_queue_id;
						event[i].event_type = RTE_EVENT_TYPE_ETHDEV;
						event[i].sub_event_type = 0;
						event[i].priority = RTE_EVENT_DEV_PRIORITY_NORMAL;
						event[i].mbuf = mbufs[i];
					}

					const int nb_tx = rte_event_enqueue_burst(eventdev_id, ev_port_id, event, nb_rx);
					if (nb_tx) {
						RTE_LOG(INFO, USER1, "RX task %u => %u events enqueued on ev-queue %u via ev-port %u\n", lcore_id, nb_tx, ev_queue_id, ev_port_id);
					}
					/* release mbufs not queued in event device */
					if (nb_tx != nb_rx) {
						for(i = nb_tx; i < nb_rx; i++) {
							RTE_LOG(WARNING, USER1, "RX task %u: dropping mbuf[%u] on port %u, queue %u\n", lcore_id, i, ev_port_id, ev_queue_id);
							rte_pktmbuf_free(mbufs[i]);
						}
					}
				}
			}
			rte_rwlock_read_unlock(&port_list_rwlock);
		}
	}
	return (int)ROFL_SUCCESS;
}


int processing_core_process_packets(void* not_used){

	unsigned int i, lcore_id = rte_lcore_id();
	uint16_t ev_port_id;
	//bool own_port = true;
	switch_port_t* port;
	wk_core_task_t* task = &wk_core_tasks[lcore_id];


	//Parsing and pipeline extra state
	datapacket_t pkt;
	datapacket_dpdk_t* pkt_state = create_datapacket_dpdk(&pkt);

	//Init values and assign
	pkt.platform_state = (platform_datapacket_state_t*)pkt_state;
	pkt_state->mbuf = NULL;

	//Set flag to active
	task->active = true;



	RTE_LOG(INFO, USER1, "run worker task on lcore_id=%d\n", lcore_id);

	while(likely(task->active)) {

		/* write event to evdev queue "ev-queue-id" via port "ev-port-id" */
		ev_port_id = task->ev_port_id;

		int timeout = 0;
		struct rte_event rx_events[PROC_ETH_TX_BURST_SIZE];
		struct rte_event tx_events[PROC_ETH_TX_BURST_SIZE];
		uint16_t nb_rx = rte_event_dequeue_burst(eventdev_id, ev_port_id, rx_events, PROC_ETH_TX_BURST_SIZE, timeout);

		if (nb_rx == 0) {
			rte_pause();
			continue;
		}

		RTE_LOG(INFO, USER1, "worker task %u => %u packets received from ev-port %u\n", lcore_id, nb_rx, ev_port_id);

		for (i = 0; i < nb_rx; i++) {

			if (rx_events[i].mbuf == NULL) {
				continue;
			}

			dpdk_port_state_t *ps;

			rte_rwlock_read_lock(&port_list_rwlock);
			if ((port = port_list[i]) == NULL) {
				rte_rwlock_read_unlock(&port_list_rwlock);
				continue;
			}

			ps = (dpdk_port_state_t *)port->platform_port_state;
			(void)ps; // just to make gcc happy for now

			int socket_id = rte_eth_dev_socket_id(ps->port_id);

			rte_rwlock_read_unlock(&port_list_rwlock);

			/* TODO: inject into openflow pipeline */

			tx_events[i].flow_id = rx_events[i].mbuf->hash.rss;
			tx_events[i].op = RTE_EVENT_OP_NEW;
			tx_events[i].sched_type = RTE_SCHED_TYPE_ATOMIC;
			tx_events[i].queue_id = task->tx_ev_queue_id[socket_id]; /* use queue-id for outgoing port's NUMA socket */
			tx_events[i].event_type = RTE_EVENT_TYPE_CPU;
			tx_events[i].sub_event_type = 0;
			tx_events[i].priority = RTE_EVENT_DEV_PRIORITY_NORMAL;
			tx_events[i].mbuf = rx_events[i].mbuf;

			rx_events[i].mbuf->udata64 = 0x0000000000000002; // outgoing port (for testing)
		}

		const int nb_tx = rte_event_enqueue_burst(eventdev_id, ev_port_id, tx_events, nb_rx);
		if (nb_tx) {
			RTE_LOG(INFO, USER1, "worker task %u => %u events enqueued via ev-port %u\n", lcore_id, nb_tx, ev_port_id);
		}
		/* release mbufs not queued in event device */
		if (nb_tx != nb_rx) {
			for(i = nb_tx; i < nb_rx; i++) {
				RTE_LOG(WARNING, USER1, "worker task %u => dropping mbuf[%u] via ev-port %u to ev-queue %u\n", lcore_id, i, ev_port_id, tx_events[i].queue_id);
				rte_pktmbuf_free(tx_events[i].mbuf);
			}
		}
	}

	destroy_datapacket_dpdk(pkt_state);

	return (int)ROFL_SUCCESS;
}


/**
 * TX packet transmission
 */
int processing_packet_transmission(void* not_used){

	unsigned int i, lcore_id = rte_lcore_id();
	//uint16_t port_id;
	//uint16_t queue_id;
	uint8_t ev_port_id;
	uint8_t ev_queue_id;
	tx_core_task_t* task = &tx_core_tasks[lcore_id];
	uint32_t out_port_id;
	int socket_id = rte_lcore_to_socket_id(lcore_id);

	//Set flag to active
	task->active = true;

	RTE_LOG(INFO, USER1, "run TX task on lcore_id %u\n", lcore_id);

	while(likely(task->active)) {

		/* write event to evdev queue "ev-queue-id" via port "ev-port-id" */
		ev_port_id = task->ev_port_id;
		ev_queue_id = task->rx_ev_queue_id;

		(void)ev_queue_id;

		int timeout = 0;
		struct rte_event events[PROC_ETH_TX_BURST_SIZE];
		uint16_t nb_rx = rte_event_dequeue_burst(eventdev_id, ev_port_id, events, PROC_ETH_TX_BURST_SIZE, timeout);

		if (nb_rx) {
			RTE_LOG(INFO, USER1, "TX task => %u packets received from ev-port %u, ev-queue %u\n", nb_rx, ev_port_id, ev_queue_id);
		}

		for (i = 0; i < nb_rx; i++) {
			switch_port_t* port;
			dpdk_port_state_t *ps;

			/* process mbuf using events[i].queue_id as pipeline stage */
			out_port_id = (uint32_t)(events[i].mbuf->udata64 & 0x00000000ffffffff);

			rte_rwlock_read_lock(&port_list_rwlock);
			if ((port = port_list[out_port_id]) == NULL) {
				rte_rwlock_read_unlock(&port_list_rwlock);
				continue;
			}

			ps = (dpdk_port_state_t *)port->platform_port_state;

			assert(out_port_id == ps->port_id);

			rte_rwlock_read_unlock(&port_list_rwlock);

			if (phyports[out_port_id].socket_id != socket_id) {
				RTE_LOG(WARNING, USER1, "TX task %u on socket %u received packet to be sent out on port %u on socket %u, dropping packet\n",
						lcore_id, socket_id, out_port_id, phyports[out_port_id].socket_id);
				rte_pktmbuf_free(events[i].mbuf);
				continue;
			}

			if (unlikely(not task->tx_queues[out_port_id].enabled)) {
				rte_pktmbuf_free(events[i].mbuf);
				continue;
			}

			unsigned int nb_tx_pkts = task->tx_queues[out_port_id].nb_tx_pkts;
			task->tx_queues[out_port_id].tx_pkts[nb_tx_pkts] = events[i].mbuf;
			task->tx_queues[out_port_id].nb_tx_pkts++;
			assert(task->tx_queues[out_port_id].nb_tx_pkts <= PROC_ETH_TX_BURST_SIZE);
		}

		for (unsigned int port_id = 0; port_id < RTE_MAX_ETHPORTS; ++port_id) {
			if ((not task->tx_queues[port_id].enabled) || (task->tx_queues[port_id].nb_tx_pkts == 0)) {
				continue;
			}

			uint16_t nb_tx = rte_eth_tx_burst(port_id, task->tx_queues[port_id].queue_id, task->tx_queues[port_id].tx_pkts, task->tx_queues[port_id].nb_tx_pkts);
			if (nb_tx != task->tx_queues[port_id].nb_tx_pkts) {
				for(i = nb_tx; i < task->tx_queues[port_id].nb_tx_pkts; i++) {
					RTE_LOG(WARNING, USER1, "TX task %u: dropping task->tx_queues[%u].tx_pkts[%u] on port %u, queue %u\n",
							lcore_id, port_id, i, port_id, task->tx_queues[port_id].queue_id);
					rte_pktmbuf_free(task->tx_queues[port_id].tx_pkts[i]);
				}
			}
			task->tx_queues[port_id].nb_tx_pkts = 0;
		}
	}

	return (int)ROFL_SUCCESS;
}


#if 0
int processing_core_process_packets(void* not_used){

	unsigned int i, l, lcore_id = rte_lcore_id();
	uint16_t port_id;
	uint16_t queue_id;
	//bool own_port = true;
	switch_port_t* port;
	//port_bursts_t* port_bursts;
    uint64_t diff_tsc, prev_tsc, cur_tsc;
	struct rte_mbuf* pkt_burst[IO_IFACE_MAX_PKT_BURST]={0};
	wk_core_task_t* task = &wk_core_tasks[lcore_id];

	//Time to drain in tics
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * IO_BURST_TX_DRAIN_US;

	if (task->n_rx_queue == 0) {
		RTE_LOG(INFO, USER1, "lcore %u has nothing to do\n", lcore_id);
		//return 0;
	}

	//Parsing and pipeline extra state
	datapacket_t pkt;
	datapacket_dpdk_t* pkt_state = create_datapacket_dpdk(&pkt);

	//Init values and assign
	pkt.platform_state = (platform_datapacket_state_t*)pkt_state;
	pkt_state->mbuf = NULL;

	//Set flag to active
	task->active = true;

	//Last drain tsc
	prev_tsc = 0;

	for (i = 0; i < task->n_rx_queue; i++) {
		port_id = task->rx_queue_list[i].port_id;
		queue_id = task->rx_queue_list[i].queue_id;
		RTE_LOG(INFO, USER1, " -- lcore_id=%u port_id=%hhu rx_queue_id=%hhu\n", lcore_id, port_id, queue_id);
	}

	RTE_LOG(INFO, USER1, "run task on lcore_id=%d\n", lcore_id);

	while(likely(task->active)){

		cur_tsc = rte_rdtsc();
		//Calc diff
		diff_tsc = cur_tsc - prev_tsc;

		//Drain TX if necessary
		if(unlikely(diff_tsc > drain_tsc)){

			//Handle physical ports
			for (i = 0, l = 0; l < total_num_of_ports && likely(i < PROCESSING_MAX_PORTS); ++i) {

				switch_port_t *port;
				dpdk_port_state_t *ps;

				rte_rwlock_read_lock(&port_list_rwlock);
				if ((port = port_list[i]) == NULL) {
					rte_rwlock_read_unlock(&port_list_rwlock);
					continue;
				}

				l++;
				ps = (dpdk_port_state_t *)port->platform_port_state;

				assert(i == ps->port_id);
#if 0
				if (task->tx_mbufs[i].len == 0) {
					rte_rwlock_read_unlock(&port_list_rwlock);
					continue;
				}
#endif
				RTE_LOG(INFO, USER1, "handle tx of core_id=%d, i=%d, ps->port_id=%d, port->name=%s\n", lcore_id, i, ps->port_id, port->name);
				// port_bursts = &task->ports[i];
				//process_port_tx(task, i);
				rte_pause();
#if 0
				task->tx_mbufs[i].len = 0;
#endif
				rte_rwlock_read_unlock(&port_list_rwlock);
			}

			prev_tsc = cur_tsc;
		}

		// Process RX
		for (unsigned int rxslot = 0; rxslot < task->n_rx_queue; ++rxslot) {

			port_id = task->rx_queue_list[rxslot].port_id;
			queue_id = task->rx_queue_list[rxslot].queue_id;

			rte_rwlock_read_lock(&port_list_rwlock);
			if ((port = port_list[port_id]) == NULL) {
				rte_rwlock_read_unlock(&port_list_rwlock);
				continue;
			}

			if (likely(port->up)) { // This CAN happen while deschedulings
				// Process RX&pipeline
				process_port_rx(lcore_id, port, port_id, queue_id, pkt_burst, &pkt, pkt_state);
			}
			rte_rwlock_read_unlock(&port_list_rwlock);
		}
	}

	destroy_datapacket_dpdk(pkt_state);

	return (int)ROFL_SUCCESS;
}
#endif

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
		XDPD_DEBUG(DRIVER_NAME"[processing][port] Stopping port %u (%s)\n", ps->port_id, port->name);
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
		if (lcores[i].is_svc_lcore){
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



