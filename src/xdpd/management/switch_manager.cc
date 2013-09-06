#include "switch_manager.h"

#include <rofl/datapath/afa/afa.h>

//Add here the headers of the version-dependant Openflow switchs 
#include "../openflow/openflow_switch.h"
#include "../openflow/openflow10/openflow10_switch.h"
#include "../openflow/openflow12/openflow12_switch.h"

using namespace rofl;

const caddress switch_manager::controller_addr = caddress(AF_INET, "127.0.0.1", 6633);
const caddress switch_manager::binding_addr = caddress(AF_INET, "0.0.0.0", 0);

//Static initialization
std::map<uint64_t, openflow_switch*> switch_manager::switchs;

/**
* Static methods of the manager
*/
openflow_switch* switch_manager::create_switch(
		of_version_t version,
		uint64_t dpid,
		std::string const& dpname,
		unsigned int num_of_tables,
		int* ma_list,
		caddress const& controller_addr,
		caddress const& binding_addr) throw (eOfSmExists, eOfSmErrorOnCreation, eOfSmVersionNotSupported){

	openflow_switch* dp;
	
	//TODO: MUTEX!

	if(switch_manager::switchs.find(dpid) != switch_manager::switchs.end()
		|| fwd_module_get_switch_by_dpid(dpid) ){
		
		throw eOfSmExists();
	}

	switch(version){

		case OF_VERSION_10:
			dp = new openflow10_switch(dpid, dpname, num_of_tables, ma_list, controller_addr, binding_addr);
			break;

		case OF_VERSION_12:
			dp = new openflow12_switch(dpid, dpname, num_of_tables, ma_list, controller_addr, binding_addr);
			break;
	
		//Add more here...
		
		default:
			throw eOfSmVersionNotSupported();

	}	
	
	//Store in the switch list
	switch_manager::switchs[dpid] = dp;

	return dp; 
}

//static
void switch_manager::destroy_switch(uint64_t dpid) throw (eOfSmDoesNotExist){

	//TODO: MUTEX!

	if (switch_manager::switchs.find(dpid) == switch_manager::switchs.end()){
		throw eOfSmDoesNotExist();
	}

	
	//Get switch instance 
	openflow_switch* dp = switch_manager::switchs[dpid];
	switch_manager::switchs.erase(dpid);

	//Destroy element
	delete dp;	
	
}

//static
void switch_manager::destroy_all_switches(){

	//TODO: MUTEX!. This is not thread safe
	//Copy and clear existing (make sure no one can recover them)
	std::map<uint64_t, openflow_switch*> copy = switchs;
	switchs.clear();

	for(std::map<uint64_t, openflow_switch*>::iterator it = copy.begin(); it != copy.end(); ++it) {
		
		//first extract it from the		
		
		//Delete
		delete it->second; 
	}
	
}
/**
 * Find the datapath by dpid 
 */
openflow_switch* switch_manager::find_by_dpid(uint64_t dpid){

	if (switch_manager::switchs.find(dpid) == switch_manager::switchs.end()){
		return NULL;
	}

	return switch_manager::switchs[dpid];
}

/**
 * Find the datapath by name 
 */
openflow_switch* switch_manager::find_by_name(std::string name){

	for(std::map<uint64_t, openflow_switch*>::iterator it = switchs.begin(); it != switchs.end(); ++it) {
		if( it->second->dpname == name)
			return it->second;	
	}

	return NULL;
}

/**
 * List datapath names
 */
//static std::vector<std::string> path_split(std::string const& words);
std::list<std::string> switch_manager::list_sw_names(void){
	
	std::list<std::string> name_list;

	for(std::map<uint64_t, openflow_switch*>::iterator it = switchs.begin(); it != switchs.end(); ++it) {
		name_list.push_back(it->second->dpname);
	}

	return name_list;
}

/* static */std::list<std::string>
switch_manager::list_matching_algorithms(of_version_t of_version)
{
	std::list<std::string> matching_algorithms;
	int i, count;

	const char * const * names;
	fwd_module_list_matching_algorithms(of_version, &names, &count);

	for (i = 0; i < count; i++) {
		matching_algorithms.push_back(std::string(names[i]));
	}

	return matching_algorithms;
}
