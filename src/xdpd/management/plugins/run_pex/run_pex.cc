#include "run_pex.h"
#include <rofl/common/utils/c_logger.h>
#include <rofl/common/croflexception.h>
#include <string>
#include <inttypes.h>
#include "../../plugin_manager.h"
#include "../../port_manager.h"
#include "../../switch_manager.h"
#include "../../pex_manager.h"

using namespace xdpd;
using namespace rofl;

#define PLUGIN_NAME "RunPEX_plugin" 

void runPEX::init()
{
	uint64_t dpid;

	ROFL_INFO("\n\n[xdpd]["PLUGIN_NAME"] **************************\n");	
	ROFL_INFO("[xdpd]["PLUGIN_NAME"] This plugin instantiates some PEX, whose description is embedded into the code.\n");
	ROFL_INFO("[xdpd]["PLUGIN_NAME"] **************************\n\n");	
	
	std::string pexName1 = "vEth25";
	std::string pexName2 = "pex11";
	
	std::string scriptPath = "/home/ivano/Desktop/pex";
	
	//Create two PEX
	try
	{
		pex_manager::create_pex_port(pexName1,pexName1,EXTERNAL);
	}catch(...)
	{
		ROFL_ERR("[xdpd]["PLUGIN_NAME"] Unable to create %s\n",pexName1.c_str());
		return;
	}
	
/*	try{
		pex_manager::create_pex(pexName2,scriptPath,0x2,4,25);
	}catch(...)
	{
		ROFL_ERR("[xdpd]["PLUGIN_NAME" Unable to create %s",pexName2.c_str());
		return;
	}
*/

/*	try{	
		ROFL_INFO("[xdpd]["PLUGIN_NAME"]Existing PEX:\n");
		std::list<std::string> available_pex = pex_manager::list_available_pex_port_names();	
		for(std::list<std::string>::iterator it = available_pex.begin(); it != available_pex.end(); it++)
		{
			ROFL_INFO("[xdpd]["PLUGIN_NAME"]\t%s\n",(*it).c_str());
		}
	}catch(...)
	{
		ROFL_ERR("[xdpd]["PLUGIN_NAME" No PEX exists\n");
		return;
	}
*/

	if(plugin_manager::get_plugins().size() != 1)
	{
		/*
		*	If at least an LSI exists, the PEX ports are connected to the first LSI of the list.
		*/
		std::list<std::string> LSIs =  switch_manager::list_sw_names();
		if(LSIs.size() != 0)
		{
			std::list<std::string>::iterator LSI = LSIs.begin();
			dpid = switch_manager::get_switch_dpid(*LSI);
			
			try
			{
				//Attach
				unsigned int port_number = 0;
				ROFL_INFO("[xdpd]["PLUGIN_NAME"] Attaching PEX port '%s' to LSI '%x'\n",pexName1.c_str(),dpid);	
				port_manager::attach_port_to_switch(dpid, pexName1, &port_number);
			}catch(...)
			{	
				ROFL_ERR("[xdpd]["PLUGIN_NAME"] Unable to attach port '%s' to LSI '%s'. Unknown error.\n", pexName1.c_str(),dpid);
				return;
			}
			//Bring up
			port_manager::bring_up(pexName1);
/*	
			try
			{
				//Attach
				unsigned int port_number = 0;
				ROFL_INFO("[xdpd]["PLUGIN_NAME"] Attaching PEX port '%s' to LSI '%x'\n",pexName2.c_str(),dpid);	
				port_manager::attach_port_to_switch(dpid, pexName2, &port_number);
			}catch(...)
			{	
				ROFL_ERR("[xdpd]["PLUGIN_NAME"] Unable to attach port '%s' to LSI '%s'. Unknown error.\n", pexName2.c_str(),dpid);
				return;
			}
			//Bring up
			port_manager::bring_up(pexName2);
	
			ROFL_INFO("[xdpd]["PLUGIN_NAME"] All the PEX have been created, and connected to an LSI.\n\n");		
*/	
		}
	}

	//Sleep some seconds before destroying the PEX		
	sleep(100);
	
	ROFL_INFO("[xdpd]["PLUGIN_NAME"] ***********************************\n");
	ROFL_INFO("[xdpd]["PLUGIN_NAME"] ***********************************\n");	
	ROFL_INFO("[xdpd]["PLUGIN_NAME"] Destroying all the PEX just created\n");	
	
	if(plugin_manager::get_plugins().size() != 1)
	{
		//Bring down the port
		port_manager::bring_down(pexName1);
		try
		{
			//Detatch
			port_manager::detach_port_from_switch(dpid, pexName1);
		}catch(...)
		{	
			ROFL_ERR("[xdpd]["PLUGIN_NAME"] Unable to detatch port '%s' from LSI '%s'. Unknown error.\n", pexName1.c_str(),dpid);
		}
/*		
		//Bring down the port
		port_manager::bring_down(pexName2);
		try
		{
			//Detatch
			port_manager::detach_port_from_switch(dpid, pexName2);
		}catch(...)
		{	
			ROFL_ERR("[xdpd]["PLUGIN_NAME"] Unable to detatch port '%s' from LSI '%s'. Unknown error.\n", pexName2.c_str(),dpid);
		}*/
	}

	try
	{
		pex_manager::destroy_pex_port(pexName1);
	}catch(...){}
/*	try
	{
		pex_manager::destroy_pex_port(pexName2);
	}catch(...){}
	*/
	if(plugin_manager::get_plugins().size() == 1)
	{	
		ROFL_INFO("[xdpd]["PLUGIN_NAME"] You have compiled xdpd with this plugin only. This xdpd execution is pretty useless, since no Logical Switch Instances will be created and there will be no way (RPC) to create them...\n\n");	
		ROFL_INFO("[xdpd]["PLUGIN_NAME"]You may now press Ctrl+C to finish xdpd execution.\n");
	}

}

