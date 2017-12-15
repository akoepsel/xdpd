#include "processing.h"
#include <utils/c_logger.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_spinlock.h>
#include <rte_rwlock.h>
#include <rte_eventdev.h>
#include <rte_bus_vdev.h>
#include <rte_service.h>
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

core_tasks_t processing_core_tasks[RTE_MAX_LCORE];
static unsigned total_num_of_ports = 0;

struct rte_mempool* direct_pools[NB_SOCKETS];

switch_port_t* port_list[PROCESSING_MAX_PORTS];
static rte_rwlock_t port_list_rwlock;

/*
 * lcore related parameters
 */
/* a set of available NUMA sockets (socket_id) */
std::set<int> sockets;
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
/* event lcores */
static uint64_t ev_coremask = 0x0002;  // lcore_id = 1
/* RX lcores */
static uint64_t rx_coremask = 0x0004;  // lcore_id = 2
/* TX lcores */
static uint64_t tx_coremask = 0x0008;  // lcore_id = 3

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
		lcores[j].is_ev_lcore = 0;
		lcores[j].is_rx_lcore = 0;
		lcores[j].is_tx_lcore = 0;
	}
	sockets.clear();
	wk_lcores.clear();

	//Get master lcore
	unsigned int master_lcore_id = rte_get_master_lcore();

	/* get svc coremask */
	YAML::Node svc_coremask_node = y_config_dpdk_ng["dpdk"]["lcores"]["svc_coremask"];
	if (svc_coremask_node && svc_coremask_node.IsScalar()) {
		svc_coremask = svc_coremask_node.as<uint64_t>();
	}

	/* get ev coremask */
	YAML::Node ev_coremask_node = y_config_dpdk_ng["dpdk"]["lcores"]["ev_coremask"];
	if (ev_coremask_node && ev_coremask_node.IsScalar()) {
		ev_coremask = ev_coremask_node.as<uint64_t>();
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

	/* detect all lcores and their state */
	for (unsigned int lcore_id = 0; lcore_id < rte_lcore_count(); lcore_id++) {
		if (lcore_id >= RTE_MAX_LCORE) {
			continue;
		}
		unsigned int socket_id = rte_lcore_to_socket_id(lcore_id);

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
			XDPD_INFO(DRIVER_NAME"[processing] skipping lcore: %u on socket: %u, role: OFF\n", lcore_id, socket_id);
		} break;
		case ROLE_SERVICE: {
			/* skip node, i.e.: do nothing */
			XDPD_INFO(DRIVER_NAME"[processing] skipping lcore: %u on socket: %u, role: SERVICE\n", lcore_id, socket_id);
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
						XDPD_ERR(DRIVER_NAME"[processing] adding lcore %u to service cores failed\n", lcore_id);
						return ROFL_FAILURE;
					};
					}
				}
				lcores[lcore_id].is_svc_lcore = 1;
				//Increase number of service lcores for this socket
				svc_lcores[socket_id].insert(lcore_id);
				s_task.assign("service lcore");
			} else
			//event lcore (=event scheduler)
			if (ev_coremask & ((uint64_t)1 << lcore_id)) {
				lcores[lcore_id].is_ev_lcore = 1;
				//Increase number of event lcores for this socket
				ev_lcores[socket_id].insert(lcore_id);
				s_task.assign("event lcore");
			} else
			//rx lcore (=packet receiving lcore)
			if (rx_coremask & ((uint64_t)1 << lcore_id)) {
				lcores[lcore_id].is_rx_lcore = 1;
				//Increase number of RX lcores for this socket
				rx_lcores[socket_id].insert(lcore_id);
				s_task.assign("RX lcore");
			} else
			//tx lcore (=packet transmitting lcore)
			if (tx_coremask & ((uint64_t)1 << lcore_id)) {
				lcores[lcore_id].is_tx_lcore = 1;
				//Increase number of TX lcores for this socket
				tx_lcores[socket_id].insert(lcore_id);
				s_task.assign("TX lcore");
			} else
			//wk lcore (=worker running openflow pipeline)
			{
				lcores[lcore_id].is_wk_lcore = 1;
				//Store socket_id in sockets
				sockets.insert(socket_id);
				//Increase number of worker lcores for this socket
				wk_lcores[socket_id].insert(lcore_id);
				s_task.assign("worker lcore");
			}

			XDPD_INFO(DRIVER_NAME" adding lcore: %u on socket: %u, enabled: %s, task: %s, next lcore is: %u, #working lcores on this socket: %u\n",
					lcore_id,
					socket_id,
					(lcores[lcore_id].is_enabled) ? "yes" : "no",
					s_task.c_str(),
					lcores[lcore_id].next_lcore_id,
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
	XDPD_DEBUG(DRIVER_NAME"[processing] Processing init: initializing eventdev device\n");

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
			XDPD_ERR(DRIVER_NAME"[processing] initialization of eventdev %s with args \"%s\" failed (EINVAL)\n", eventdev_name.c_str(), eventdev_args.c_str());
		} break;
		case -EEXIST: {
			XDPD_ERR(DRIVER_NAME"[processing] Processing init: initializing eventdev %s failed (EEXIST)\n", eventdev_name.c_str());
		} break;
		case -ENOMEM: {
			XDPD_ERR(DRIVER_NAME"[processing] initialization of eventdev %s with args \"%s\" failed (ENOMEM)\n", eventdev_name.c_str(), eventdev_args.c_str());
		} break;
		default: {
			XDPD_ERR(DRIVER_NAME"[processing] initialization of eventdev %s with args \"%s\" failed\n", eventdev_name.c_str(), eventdev_args.c_str());
		};
		}
		return ROFL_FAILURE;
	}
	uint8_t nb_event_devs = rte_event_dev_count();
	XDPD_DEBUG(DRIVER_NAME"[processing] Processing init: %u eventdev device(s) available\n", nb_event_devs);

	/* get eventdev id */
	eventdev_id = rte_event_dev_get_dev_id(eventdev_name.c_str());

	/* get eventdev info structure */
	if ((ret = rte_event_dev_info_get(eventdev_id, &eventdev_info)) < 0) {
		rte_exit(1, "unable to retrieve info struct for eventdev %s\n", eventdev_name.c_str());
	}

	XDPD_DEBUG(DRIVER_NAME"[processing] Processing init: eventdev: %s max_event_ports: %u max_event_queues: %u\n",
			eventdev_name.c_str(), eventdev_info.max_event_ports, eventdev_info.max_event_queues);


	/* configure event device */
	memset(&eventdev_conf, 0, sizeof(eventdev_conf));
	eventdev_conf.nb_event_queues = 2; /* RX =(queue)=> worker =(queue)=> TX */
	eventdev_conf.nb_event_ports = 0;
	for (auto it : tx_lcores) {
		eventdev_conf.nb_event_ports += it.second.size(); /* number of all TX lcores on all NUMA sockets */
	}
	for (auto it : wk_lcores) {
		eventdev_conf.nb_event_ports += it.second.size(); /* number of all worker lcores on all NUMA sockets */
	}
	if (eventdev_conf.nb_event_ports > eventdev_info.max_event_ports) {
		XDPD_ERR(DRIVER_NAME"[processing] initialization of eventdev %s failed, too many event ports required\n", eventdev_name.c_str());
		return ROFL_FAILURE;
	}
	eventdev_conf.nb_events_limit = eventdev_info.max_num_events;
	eventdev_conf.nb_event_queue_flows = eventdev_info.max_event_queue_flows;
	eventdev_conf.nb_event_port_dequeue_depth = eventdev_info.max_event_port_dequeue_depth;
	eventdev_conf.nb_event_port_enqueue_depth = eventdev_info.max_event_port_enqueue_depth;

	XDPD_DEBUG(DRIVER_NAME"[processing] Processing init: configuring eventdev: %s, nb_event_queues: %u, nb_event_ports: %u, nb_events_limit: %u, nb_event_queue_flows: %u, nb_event_port_dequeue_depth: %u, nb_event_port_enqueue_depth: %u\n",
			eventdev_name.c_str(), eventdev_conf.nb_event_queues, eventdev_conf.nb_event_ports,
			eventdev_conf.nb_events_limit, eventdev_conf.nb_event_queue_flows,
			eventdev_conf.nb_event_port_dequeue_depth, eventdev_conf.nb_event_port_enqueue_depth);

	if ((ret = rte_event_dev_configure(eventdev_id, &eventdev_conf)) < 0) {
		XDPD_ERR(DRIVER_NAME"[processing] initialization of eventdev %s, rte_event_dev_configure() failed\n", eventdev_name.c_str());
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

		/* nb_atomic_flows */
		YAML::Node nb_atomic_order_sequences_node = y_config_dpdk_ng["dpdk"]["eventdev"]["queues"][queue_id]["nb_atomic_order_sequences_node"];
		if (nb_atomic_order_sequences_node && nb_atomic_order_sequences_node.IsScalar()) {
			queue_conf.nb_atomic_order_sequences = nb_atomic_order_sequences_node.as<uint8_t>();
		} else {
			queue_conf.nb_atomic_order_sequences = eventdev_conf.nb_event_queue_flows;
		}

		if (rte_event_queue_setup(eventdev_id, queue_id, &queue_conf) < 0) {
			XDPD_ERR(DRIVER_NAME"[processing] initialization of eventdev %s, rte_event_queue_setup() on queue_id: %u failed\n", eventdev_name.c_str(), queue_id);
			return ROFL_FAILURE;
		}
	}


	/* configure event ports for worker lcores */
	for (unsigned int port_id = 0; port_id < wk_lcores.size(); port_id++) {
		struct rte_event_port_conf port_conf;
		memset(&port_conf, 0, sizeof(port_conf));
		port_conf.dequeue_depth = eventdev_conf.nb_event_port_dequeue_depth;
		port_conf.enqueue_depth = eventdev_conf.nb_event_port_enqueue_depth;
		port_conf.new_event_threshold = eventdev_conf.nb_events_limit;

		if (rte_event_port_setup(eventdev_id, port_id, &port_conf) < 0) {
			XDPD_ERR(DRIVER_NAME"[processing] initialization of eventdev %s, rte_event_port_setup() on port_id: %u failed\n", eventdev_name.c_str(), port_id);
			return ROFL_FAILURE;
		}

		/* link up event port and queues */
		uint8_t queues[] = {0};

		if (rte_event_port_link(eventdev_id, port_id, queues, NULL, sizeof(queues)) < 0) {
			XDPD_ERR(DRIVER_NAME"[processing] initialization of eventdev %s, rte_event_port_link() on port_id: %u failed\n", eventdev_name.c_str(), port_id);
			return ROFL_FAILURE;
		}
	}


	/* configure event ports for TX lcores */
	for (unsigned int port_id = wk_lcores.size(); port_id < (wk_lcores.size() + tx_lcores.size()); port_id++) {
		struct rte_event_port_conf port_conf;
		memset(&port_conf, 0, sizeof(port_conf));
		port_conf.dequeue_depth = eventdev_conf.nb_event_port_dequeue_depth;
		port_conf.enqueue_depth = eventdev_conf.nb_event_port_enqueue_depth;
		port_conf.new_event_threshold = eventdev_conf.nb_events_limit;

		if (rte_event_port_setup(eventdev_id, port_id, &port_conf) < 0) {
			XDPD_ERR(DRIVER_NAME"[processing] initialization of eventdev %s, rte_event_port_setup() on port_id: %u failed\n", eventdev_name.c_str(), port_id);
			return ROFL_FAILURE;
		}

		/* link up event port and queues */
		uint8_t queues[] = {1};

		if (rte_event_port_link(eventdev_id, port_id, queues, NULL, sizeof(queues)) < 0) {
			XDPD_ERR(DRIVER_NAME"[processing] initialization of eventdev %s, rte_event_port_link() on port_id: %u failed\n", eventdev_name.c_str(), port_id);
			return ROFL_FAILURE;
		}
	}

	/* get event device service_id for service core */
	uint32_t service_id = 0;
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
			XDPD_DEBUG(DRIVER_NAME"[processing] mapping service %s (%u) for eventdev %s to service lcore %u\n",
									rte_service_get_name(service_id), service_id, eventdev_name.c_str(), lcore_id);
			if ((ret = rte_service_map_lcore_set(service_id, lcore_id, /*enable=*/1)) < 0) {
				XDPD_ERR(DRIVER_NAME"[processing] mapping of service %s (%u) for eventdev %s to service lcore %u failed\n",
						rte_service_get_name(service_id), service_id, eventdev_name.c_str(), lcore_id);
				return ROFL_FAILURE;
			}
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
	memset(processing_core_tasks, 0, sizeof(processing_core_tasks));
	memset(port_list, 0, sizeof(port_list));

	/*
	 * set log level
	 */
	YAML::Node log_level_node = y_config_dpdk_ng["dpdk"]["log_level"];
	if (log_level_node && log_level_node.IsScalar()) {
		rte_log_set_global_level(log_level_node.as<uint32_t>());
	}

	/*
	 * discover lcores
	 */
	if (ROFL_FAILURE == processing_init_lcores()) {
		rte_exit(1, "RTE lcore discovery failed\n");
	}

	/*
	 * initialize RTE event device
	 */
	if (ROFL_FAILURE == processing_init_eventdev()) {
		rte_exit(1, "RTE event device initialization failed\n");
	}

	//Initialize basics
	max_cores = rte_lcore_count();

	rte_rwlock_init(&port_list_rwlock);

	XDPD_DEBUG(DRIVER_NAME"[processing] Processing init: %u logical cores guessed from rte_eal_get_configuration(). Master is: %u\n", rte_lcore_count(), rte_get_master_lcore());
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

			if(lcore_id == rte_get_master_lcore()){
				continue;
			}

			processing_core_tasks[lcore_id].available = true;
			XDPD_DEBUG(DRIVER_NAME"[processing] Marking core %u as available\n", lcore_id);

#if 0
			//Recover CPU socket for the lcore
			unsigned int socket_id = rte_lcore_to_socket_id(lcore_id);

			if(direct_pools[socket_id] == NULL){

				/**
				*  create the mbuf pool for that socket id
				*/
				char pool_name[RTE_MEMPOOL_NAMESIZE];
				snprintf (pool_name, RTE_MEMPOOL_NAMESIZE, "pool_direct_%u", socket_id);
				XDPD_INFO(DRIVER_NAME"[processing] Creating mempool %s with %u mbufs each of size %u bytes for CPU socket %u\n", pool_name, mbuf_elems_in_pool, mbuf_data_room_size, socket_id);

				direct_pools[socket_id] = rte_pktmbuf_pool_create(
						pool_name,
						/*number of elements in pool=*/mbuf_elems_in_pool,
						/*cache_size=*/0,
						/*priv_size=*/RTE_ALIGN(sizeof(struct rte_pktmbuf_pool_private), RTE_MBUF_PRIV_ALIGN),
						/*data_room_size=*/mbuf_data_room_size,
						socket_id);

				if (direct_pools[socket_id] == NULL) {
					XDPD_INFO(DRIVER_NAME"[processing] Unable to allocate mempool %s due to error \"%s\"\n", pool_name, rte_strerror(rte_errno));
					switch (rte_errno) {
					case E_RTE_NO_CONFIG: {

					} break;
					case E_RTE_SECONDARY: {

					} break;
					case EINVAL: {

					} break;
					case ENOSPC: {

					} break;
					case EEXIST: {

					} break;
					case ENOMEM: {

					} break;
					default: {

					};
					}

					rte_panic("Cannot init direct mbuf pool for CPU socket: %u\n", socket_id);
				}
			}
#endif
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
		if ((ret = rte_service_lcore_start(lcore_id)) < 0) {
			switch (ret) {
			case -EALREADY: {
				/* do nothing */
			} break;
			default: {
				XDPD_ERR(DRIVER_NAME"[processing] start of service lcore %u failed\n", lcore_id);
			} return ROFL_FAILURE;
			}
		}
	}

	/* start event device */
	if (rte_event_dev_start(eventdev_id) < 0) {
		XDPD_ERR(DRIVER_NAME"[processing] initialization of eventdev %s, rte_event_dev_start() failed\n", eventdev_name.c_str());
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

		/* event scheduler lcores */
		if (lcores[lcore_id].is_ev_lcore) {
			// TODO
		}

		/* transmitting lcores */
		if (lcores[lcore_id].is_tx_lcore) {
			// TODO
		}

		/* receiving lcores */
		if (lcores[lcore_id].is_rx_lcore) {
			// TODO
		}

		/* worker lcores */
		if (lcores[lcore_id].is_wk_lcore) {

			// lcore already running?
			if (processing_core_tasks[lcore_id].active == true) {
				continue;
			}

			// lcore should be in state WAIT
			if (rte_eal_get_lcore_state(lcore_id) != WAIT) {
				XDPD_ERR(DRIVER_NAME "[processing][init] ignoring core %u for launching, out of sync (task state != WAIT)\n", lcore_id);
				continue;
			}

			XDPD_DEBUG(DRIVER_NAME "[processing][init] starting lcore %u (%u)\n", lcore_id, processing_core_tasks[lcore_id].n_rx_queue);

			// launch processing task on lcore
			if (rte_eal_remote_launch(&processing_core_process_packets, NULL, lcore_id)) {
				XDPD_ERR(DRIVER_NAME "[processing][init] ignoring lcore %u for starting, as it is not waiting for new task\n", lcore_id);
				continue;
			}

			processing_core_tasks[lcore_id].active = true;
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
		if(processing_core_tasks[lcore_id].available && processing_core_tasks[lcore_id].active){
			XDPD_DEBUG(DRIVER_NAME"[processing][shutdown] Shutting down active lcore %u\n", lcore_id);
			processing_core_tasks[lcore_id].active = false;
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


int processing_core_process_packets(void* not_used){

	unsigned int i, l, lcore_id = rte_lcore_id();
	uint16_t port_id;
	uint16_t queue_id;
	//bool own_port = true;
	switch_port_t* port;
	//port_bursts_t* port_bursts;
    uint64_t diff_tsc, prev_tsc, cur_tsc;
	struct rte_mbuf* pkt_burst[IO_IFACE_MAX_PKT_BURST]={0};
	core_tasks_t* task = &processing_core_tasks[lcore_id];

	//Time to drain in tics
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * IO_BURST_TX_DRAIN_US;

	if (task->n_rx_queue == 0) {
		RTE_LOG(INFO, XDPD, "lcore %u has nothing to do\n", lcore_id);
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
		RTE_LOG(INFO, XDPD, " -- lcore_id=%u port_id=%hhu rx_queue_id=%hhu\n", lcore_id, port_id, queue_id);
	}

	RTE_LOG(INFO, XDPD, "run task on lcore_id=%d\n", lcore_id);

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

				if (task->tx_mbufs[i].len == 0) {
					rte_rwlock_read_unlock(&port_list_rwlock);
					continue;
				}

				RTE_LOG(INFO, XDPD, "handle tx of core_id=%d, i=%d, ps->port_id=%d, port->name=%s\n", lcore_id, i, ps->port_id, port->name);
				// port_bursts = &task->ports[i];
				process_port_tx(task, i);
				task->tx_mbufs[i].len = 0;
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
		XDPD_DEBUG(DRIVER_NAME"[processing][port] Starting port %u (%s)\n", ps->port_id, port->name);
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
	core_tasks_t* core_task;
	std::stringstream ss;
	enum rte_lcore_role_t role;
	enum rte_lcore_state_t state;

	ss << DRIVER_NAME"[processing] Core status:" << std::endl;

	for(i=0;i<RTE_MAX_LCORE;++i){
		core_task = &processing_core_tasks[i];

		if(i && !core_task->available)
			continue;

		//Print basic info
		ss << "\t socket (" << rte_lcore_to_socket_id(i) << ")";

		ss << " core (" << i << ")";

		if(i == 0){
			ss << " Master"<<std::endl;
			continue;
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
				ss << "SERVICE";
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



