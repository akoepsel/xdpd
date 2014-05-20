#ifndef NODE_ORCHESTRATOR_PLUGIN_H_
#define NODE_ORCHESTRATOR_PLUGIN_H_	1

#pragma once

#include "../../plugin_manager.h"
#include "../../port_manager.h"
#include "../../pex_manager.h"
#include "../../switch_manager.h"

#include <rofl/common/logging.h>
#include <rofl/datapath/pipeline/openflow/of_switch.h>
#include "../../../openflow/openflow_switch.h"
#include "LSI.h"
#include "orchestrator_constants.h"

#include <list>
#include <map>
#include <pthread.h>

#include "server.h"

using namespace std;

/**
* @file node_orchestrator.h
* @author Ivano Cerrato<ivano.cerrato (at) polito.it>
*
* @brief Plugin that receives commands from the node orchestrator
* 
*/

class openflow_switch;
class Server;

namespace xdpd {

/**
* @brief Plugin that receives command from the node orchestrator
* @ingroup cmm_mgmt_plugins
*/
class NodeOrchestrator:public plugin {
	
friend class Server;
	
public:
	virtual void init(void);

	virtual std::string get_name(void)
	{
		return std::string(PLUGIN_NAME);
	};

protected:
	/*
	*	Methods implementing commands from the orchestrator
	*/
	static LSI createLSI(list<string> phyPorts, string controllerAddress, string controllerPort);
	static list<string> discoverPhyPorts();
	static pair<unsigned int, unsigned int> createVirtualLink(uint64_t dpid_a,uint64_t dpid_b);
	static unsigned int createNfPort(uint64_t dpid, string NfName,PexType type);
	
	static uint64_t nextDpid;
	
private:
	//FIXME: tmp because the xDPD api does not export the ID of vlinks
	//for each LSI, contains the last ID used as port identifier
	static map<uint64_t, unsigned int> last_ports_id;
};

}// namespace xdpd 

#endif /* NODE_ORCHESTRATOR_PLUGIN_H_ */


