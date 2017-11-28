#include "processing.h"
#include <utils/c_logger.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_spinlock.h>
#include <rte_rwlock.h>
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
* Initialize data structures for processing to work
*/
rofl_result_t processing_init(void){

	//Cleanup
	memset(direct_pools, 0, sizeof(direct_pools));
	memset(processing_core_tasks, 0, sizeof(processing_core_tasks));
	memset(port_list, 0, sizeof(port_list));

	YAML::Node log_level_node = y_config_dpdk_ng["dpdk"]["log_level"];
	if (log_level_node && log_level_node.IsScalar()) {
		rte_log_set_global_level(log_level_node.as<uint32_t>());
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

	for (unsigned int lcore_id = 0; lcore_id < rte_lcore_count(); lcore_id++) {
		// sanity check
		if (lcore_id >= RTE_MAX_LCORE) {
			continue;
		}
		// do not start anything on master lcore
		if (lcore_id == rte_get_master_lcore()) {
			continue;
		}

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

	//Print the status of the cores
	processing_dump_core_states();

	return ROFL_SUCCESS;
}


/*
* Destroy data structures for processing to work
*/
rofl_result_t processing_destroy(void){

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



