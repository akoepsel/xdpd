/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _PLATFORM_PORT_MGMT_
#define _PLATFORM_PORT_MGMT_

#include <rofl/datapath/afa/fwd_module.h>

/**
* @file iface_utils.h
* @author Victor Alvarez<victor.alvarez (at) bisdn.de>
*
* @brief Network interface (port) utils.
* 
*/


//C++ extern C
ROFL_BEGIN_DECLS

/*
 * Get the port by its name
 */
switch_port_t* get_port_by_name(const char *name);

/*
 * Looks in the platform for physical ports and fills up the switch_port_t sructure with them
 *
 */
afa_result_t discover_physical_ports(void);

/*
 * Destroy port 
 */
afa_result_t destroy_port(switch_port_t* port);

//Port enable/disable
/*
* Enable port (direct call to ioport instance)
*/
afa_result_t enable_port(platform_port_state_t* ioport_instance);

/*
* Disable port (direct call to ioport instance)
*/
afa_result_t disable_port(platform_port_state_t* ioport_instance);

//C++ extern C
ROFL_END_DECLS

#endif //PLATFORM_PORT_MGMT
