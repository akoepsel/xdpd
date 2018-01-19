/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _RX_H_
#define _RX_H_

#include "../config.h"
#include <utils/c_logger.h>
#include <rte_config.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_spinlock.h>
#include <rte_eal.h>
#include <rte_mbuf.h>
#include <rte_ethdev.h>

#include "assert.h"
#include "dpdk_datapacket.h"

#include "port_state.h"
#include "iface_manager.h"
#include "../processing/processing.h"

//Make sure pipeline-imp are BEFORE _pp.h
//so that functions can be inlined
#include "../pipeline-imp/rte_atomic_operations.h"
#include "../pipeline-imp/lock.h"
#include "../pipeline-imp/packet.h"

#include <iostream>

//Now include pp headers
#include <rofl/datapath/pipeline/openflow/of_switch_pp.h>

namespace xdpd {
namespace gnu_linux_dpdk_ng {

//
// RX processing
//


/*
* Processes RX in a specific port. The function will process up to MAX_BURST_SIZE
*/
inline void
rx_pkt(unsigned int lcore_id, of_switch_t* sw, struct rte_mbuf* mbuf, datapacket_t* pkt, datapacket_dpdk_t* pkt_state){

	unsigned int i = 0;
	switch_port_t* tmp_port;
	datapacket_dpdk_t* pkt_dpdk = pkt_state;

	if(unlikely(mbuf == NULL)){
		return;
	}

	if(unlikely(sw == NULL)){
		rte_pktmbuf_free(mbuf);
		return;
	}

	//set mbuf pointer in the state so that it can be recovered afterwards when going
	//out from the pipeline
	pkt_state->mbuf = mbuf;

	//We only support nb_segs == 1. TODO: can it be that NICs send us pkts with more than one segment?
	assert(mbuf->nb_segs == 1);

	tmp_port = phy_port_mapping[mbuf->port];

	if(unlikely(!tmp_port)){
		//Not attached
		rte_pktmbuf_free(mbuf);
		return;
	}

	//Init&classify
	init_datapacket_dpdk(pkt_dpdk, mbuf, sw, tmp_port->of_port_num, 0, true, false);

	XDPD_DEBUG("calling of_process_packet_pipeline i=%d core_id=%d (%p)\n", i, lcore_id, pkt);

	//Send to pipeline
	of_process_packet_pipeline(lcore_id, sw, pkt);
}


}// namespace xdpd::gnu_linux_dpdk_ng
}// namespace xdpd

#endif //_RX_H_
