#include "ports.h"
#include <stdio.h>
#include <rofl/datapath/pipeline/physical_switch.h>
#include <rofl/datapath/pipeline/switch_port.h>

#define FWD_MOD_NAME "netfpga10g"

//Discover ports and initialize, raw sockets and I/O thread
rofl_result_t netfpga_discover_ports(){
	unsigned int i;	
	switch_port_t* port;
	char iface_name[NETFPGA_INTERFACE_NAME_LEN] = "0"; //nfX\0

	//Just add stuff
	for(i=0; i< NETFPGA_NUM_PORTS; ++i){
		//Compose name nf0...nf3
		snprintf(iface_name, NETFPGA_INTERFACE_NAME_LEN, NETFPGA_INTERFACE_BASE_NAME"%d", i);
		
		ROFL_DEBUG("["FWD_MOD_NAME"] Attempting to discover %s\n", iface_name);
	
		//FIXME: interfaces should be anyway checked, and set link up.. but anyway. First implementation	
		port = switch_port_init(iface_name, true/*will be overriden afterwards*/, PORT_TYPE_PHYSICAL, PORT_STATE_LIVE);

		//XXX FIXME init dev
		
		//Add to available ports
		if( physical_switch_add_port(port) != ROFL_SUCCESS )
			return ROFL_FAILURE;
		
	}


	return ROFL_SUCCESS;		
}

rofl_result_t netfpga_attach_ports(of_switch_t* sw){

	unsigned int i, of_port_num;
	switch_port_t* port;
	char iface_name[NETFPGA_INTERFACE_NAME_LEN] = "0"; //nfX\0

	//Just attach 
	for(i=0; i< NETFPGA_NUM_PORTS; ++i){
		//Compose name nf0...nf3
		snprintf(iface_name, NETFPGA_INTERFACE_NAME_LEN, NETFPGA_INTERFACE_BASE_NAME"%d", i);
		
		ROFL_DEBUG("["FWD_MOD_NAME"] Attempting to attach %s\n", iface_name);
	
		//FIXME: interfaces should be anyway checked, and set link up.. but anyway. First implementation	
		port = physical_switch_get_port_by_name(iface_name);
	
		//Do the attachment	
		if(physical_switch_attach_port_to_logical_switch(port, sw, &of_port_num) == ROFL_FAILURE)
			return ROFL_FAILURE;
		
	
	}
	return ROFL_SUCCESS;
}

