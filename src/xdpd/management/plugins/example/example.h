#ifndef EXAMPLE_PLUGIN_H
#define EXAMPLE_PLUGIN_H 

#include "../../plugin_manager.h"

/**
* @file example_plugin.h
* @author Marc Sune<marc.sune (at) bisdn.de>
*
* @brief Simple example of a plugin
* 
*/

namespace xdpd {

class example:public plugin {
	
public:
	virtual void init(int args, char** argv);

	virtual std::string get_name(void){
		return std::string("Example plugin");
	};
};

}// namespace xdpd 

#endif /* EXAMPLE_PLUGIN_H_ */


