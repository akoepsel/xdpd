/*
 * openflow10_switch.h
 *
 *  Created on: 06.09.2013
 *      Author: andreas
 */

#ifndef OPENFLOW10_SWITCH_H_
#define OPENFLOW10_SWITCH_H_

#include <rofl/datapath/pipeline/openflow/openflow12/of12_switch.h>
#include <rofl/datapath/afa/fwd_module.h>
#include <rofl/datapath/afa/openflow/openflow12/of12_fwd_module.h>
#include "../openflow_switch.h"
#include "of10_endpoint.h"

/**
* @file openflow12_switch.h
* @author Andreas Koepsel<andreas.koepsel (at) bisdn.de>
* @author Marc Sune<marc.sune (at) bisdn.de>
*
* @brief openflowf12_switch is the representation of an Openflow v1.2
* switch (logical switch).
*
* The switch contains the of12_endpoint, but may contain other more.
*
*/

namespace rofl {


class openflow10_switch : public openflow_switch {


public:
	/**
	 * Constructor
	 */
	openflow10_switch(uint64_t dpid,
				std::string const& dpname,
				unsigned int num_of_tables,
				int* ma_list,
				caddress const& controller_addr = caddress(AF_INET, "127.0.0.1", 6633),
				caddress const& binding_addr = caddress(AF_INET, "0.0.0.0", 0)) throw (eOfSmVersionNotSupported);


	/**
	 * Destructor
	 */
	virtual ~openflow10_switch(void);

	/*
	* Public interface for the instance
	*/

	afa_result_t process_packet_in(uint8_t table_id,
					uint8_t reason,
					uint32_t in_port,
					uint32_t buffer_id,
					uint8_t* pkt_buffer,
					uint32_t buf_len,
					uint16_t total_len,
					of12_packet_matches_t matches);

	afa_result_t process_flow_removed(uint8_t reason, of12_flow_entry_t* removed_flow_entry);

	/*
	* Port async notifications
	*/
	virtual afa_result_t notify_port_add(switch_port_t* port);

	virtual afa_result_t notify_port_delete(switch_port_t* port);

	virtual afa_result_t notify_port_status_changed(switch_port_t* port);

};

}// namespace rofl


#endif /* OPENFLOW10_SWITCH_H_ */
