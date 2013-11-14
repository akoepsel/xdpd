#include "port_manager.h"
#include <rofl/datapath/afa/cmm.h>
#include <rofl/common/utils/c_logger.h>
#include <rofl/datapath/pipeline/openflow/of_switch.h>
#include <rofl/datapath/pipeline/common/datapacket.h>
#include <rofl/datapath/pipeline/physical_switch.h>

#include "port_state.h"

#include "../config.h"
#include <assert.h> 
#include <rte_common.h> 
#include <rte_malloc.h> 
#include <rte_errno.h> 

extern struct rte_mempool *pool_direct;
switch_port_t* port_mapping[PORT_MANAGER_MAX_PORTS] = {0};

//Initializes the pipeline structure and launches the port 
static switch_port_t* configure_port(unsigned int port_id){

	int ret;
	switch_port_t* port;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_conf port_conf;
	char port_name[SWITCH_PORT_MAX_LEN_NAME];
	
	//Get info
	rte_eth_dev_info_get(port_id, &dev_info);

	//Hack to "deduce" the maximum speed of the NIC.
	//As of DPDK v1.4 there is not way to retreive such features from
	//the NIC
	if( strncmp(dev_info.driver_name, "rte_ixgbe", 9) == 0 ){
		/* 10G */
		snprintf (port_name, SWITCH_PORT_MAX_LEN_NAME, "10ge%u",port_id);
	}else{
		/* 1G */
		snprintf (port_name, SWITCH_PORT_MAX_LEN_NAME, "ge%u",port_id);
	}

	//Initialize pipeline port
	port = switch_port_init(port_name, true/*will be overriden afterwards*/, PORT_TYPE_PHYSICAL, PORT_STATE_LIVE);
	if(!port)
		return NULL; 

	//Generate port state
	dpdk_port_state_t* ps = (dpdk_port_state_t*)rte_malloc(NULL,sizeof(dpdk_port_state_t),0);
	
	if(!ps){
		switch_port_destroy(port);
		return NULL;
	}

	//Mark link status as automatic
	//TODO	
	
	//Set rx and tx queues
	memset(&port_conf, 0, sizeof(port_conf));
	port_conf.rxmode.max_rx_pkt_len =  IO_MAX_PACKET_SIZE;
	//port_conf.rxmode.hw_ip_checksum = 1;
	//port_conf.rx_adv_conf.rss_conf.rss_hf = ETH_RSS_IPV4 | ETH_RSS_IPV6;
	port_conf.txmode.mq_mode = ETH_MQ_TX_NONE;
	if ((ret=rte_eth_dev_configure(port_id, 1, IO_IFACE_NUM_QUEUES, &port_conf)) < 0){
		assert(0);
		ROFL_ERR("Cannot configure device: %s (%s)\n", port->name, rte_strerror(ret));
		return NULL;
	}


	//Fill-in dpdk port state
	ps->scheduled = false;
	ps->port_id = port_id;
	port->platform_port_state = (platform_port_state_t*)ps;

	ROFL_INFO("Discovered port %s [%u:%u:%u] id %u\n", port_name, dev_info.pci_dev->addr.domain, dev_info.pci_dev->addr.bus, dev_info.pci_dev->addr.devid, port_id);

	//Set the port in the port_mapping
	port_mapping[port_id] = port;

	return port;
}

rofl_result_t port_manager_set_queues(unsigned int core_id, unsigned int port_id){
	
	unsigned int i;
	int ret;
	struct rte_eth_rxconf rx_conf = {
		.rx_thresh = {
			.pthresh = RX_PTHRESH,
			.hthresh = RX_HTHRESH,
			.wthresh = RX_WTHRESH,
		},
		.rx_free_thresh = 32,
	};
	struct rte_eth_txconf tx_conf;
	tx_conf.tx_thresh.pthresh = TX_PTHRESH;
	tx_conf.tx_thresh.hthresh = TX_HTHRESH;
	tx_conf.tx_thresh.wthresh = TX_WTHRESH;
	tx_conf.tx_free_thresh = 0; /* Use PMD default values */
	tx_conf.tx_rs_thresh = 0; /* Use PMD default values */
	
	//Set RX
	if( (ret=rte_eth_rx_queue_setup(port_id, 0, RTE_TEST_RX_DESC_DEFAULT, rte_eth_dev_socket_id(port_id), &rx_conf, pool_direct)) < 0 ){
		ROFL_ERR("Cannot setup RX queue: %s\n", rte_strerror(ret));
		assert(0);
		return ROFL_FAILURE;
	}

	//Set TX
	for(i=0;i<IO_IFACE_NUM_QUEUES;++i){
		if( (ret = rte_eth_tx_queue_setup(port_id, i, RTE_TEST_TX_DESC_DEFAULT, rte_eth_dev_socket_id(port_id), &tx_conf)) < 0 ){
	 
			ROFL_ERR("Cannot setup TX queues: %s\n", rte_strerror(ret));
			assert(0);
			return ROFL_FAILURE;
		}
	}
	//Start port
	if((ret=rte_eth_dev_start(port_id)) < 0){
		ROFL_ERR("Cannot start device %u:  %s\n", port_id, rte_strerror(ret));
		assert(0);
		return ROFL_FAILURE; 
	}

	//Set promiscuous mode
	rte_eth_promiscuous_enable(port_id);

	//Enable multicast
	rte_eth_allmulticast_enable(port_id);
	
	//Reset stats
	rte_eth_stats_reset(port_id);
	
	return ROFL_SUCCESS;
}

/*
* Discovers and initializes (including rofl-pipeline state) DPDK-enabled ports.
*/
rofl_result_t port_manager_discover_system_ports(void){

	uint8_t i, num_of_ports;
	switch_port_t* port;
	num_of_ports = rte_eth_dev_count();
	
	ROFL_INFO("Found %u DPDK-capable interfaces\n", num_of_ports);
	
	for(i=0;i<num_of_ports;++i){
		if(! ( port = configure_port(i) ) ){
			ROFL_ERR("Unable to initialize port-id: %u\n", i);
			return ROFL_FAILURE;
		}

		//Add port to the pipeline
		if( physical_switch_add_port(port) != ROFL_SUCCESS ){
			ROFL_ERR("Unable to add the switch port to physical switch; perhaps there are no more physical port slots available?\n");
			return ROFL_FAILURE;
		}

	}	

	return ROFL_SUCCESS;
}

/*
* Shutdown all ports in the system 
*/
rofl_result_t port_manager_shutdown_ports(void){

	uint8_t i, num_of_ports;
	num_of_ports = rte_eth_dev_count();
	
	for(i=0;i<num_of_ports;++i){
		rte_eth_dev_stop(i);
	}	

	return ROFL_SUCCESS;
}

/*
* Update link states 
*/
void port_manager_update_links(){

	unsigned int i;
	struct rte_eth_link link;
	switch_port_t* port;
	bool last_link_state;
	
	for(i=0;i<PORT_MANAGER_MAX_PORTS;i++){
		
		port = port_mapping[i];
		
		if(unlikely(port != NULL)){
			rte_eth_link_get_nowait(i,&link);
	
			last_link_state = !((port->state& PORT_STATE_LINK_DOWN) > 0); //up =>1

			//Check if there has been a change
			if(unlikely(last_link_state != link.link_status)){
				if(link.link_status)
					//Up
					port->state = port->state & ~(PORT_STATE_LINK_DOWN); 
				else
					//Down
					port->state = port->state | PORT_STATE_LINK_DOWN;
					
				ROFL_DEBUG("[port-manager] Port %s is %s, and link is %s\n", port->name, ((port->up) ? "up" : "down"), ((link.link_status) ? "detected" : "not detected"));
				
				//Notify CMM port change
				if(cmm_notify_port_status_changed(port) != AFA_SUCCESS){
					ROFL_ERR("Unable to notify port status change for port %s\n", port->name);
				}	
			}
		}
	}
}

/*
* Update port stats (pipeline)
*/
void port_manager_update_stats(){
	
	unsigned int i, j;
	struct rte_eth_stats stats;
	switch_port_t* port;
	
	for(i=0;i<PORT_MANAGER_MAX_PORTS;i++){
		port = port_mapping[i];
		if(unlikely(port != NULL)){

			//Retrieve stats
			rte_eth_stats_get(i, &stats);
			
			//RX	
			port->stats.rx_packets = stats.ipackets;
			port->stats.rx_bytes = stats.ibytes;
			port->stats.rx_errors = stats.ierrors;
				
			//FIXME: collisions and other errors
		
			//TX
			port->stats.tx_packets = stats.opackets;
			port->stats.tx_bytes = stats.obytes;
			port->stats.tx_errors = stats.oerrors;

			//TX-queues
			for(j=0;j<IO_IFACE_NUM_QUEUES;j++){
				port->queues[j].stats.tx_packets = stats.q_opackets[j];
				port->queues[j].stats.tx_bytes = stats.q_obytes[j];
				//port->queues[j].stats.overrun = stats.q_;
			}
		}
	}

}
