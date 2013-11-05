/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _TX_RX_H_
#define _TX_RX_H_

#define __STDC_LIMIT_MACROS
#include "../config.h"
#include <rofl/common/utils/c_logger.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_spinlock.h>
#include <rte_eal.h> 
#include <rte_mbuf.h> 
#include <rte_ethdev.h> 

#include "assert.h"
#include "datapacketx86.h"
#include "dpdk_datapacket.h"

#include "port_state.h"
#include "../processing/processing.h"
#include <rofl/datapath/pipeline/openflow/of_switch.h>

namespace xdpd {
namespace gnu_linux_dpdk {

//
// Packet processing
//


/*
* Processes RX in a specific port. The function will process up to MAX_BURST_SIZE 
*/
inline void process_port_rx(switch_port_t* port, unsigned int port_id, struct rte_mbuf** pkts_burst, datapacket_t* pkt, dpdk_pkt_platform_state_t* pkt_state){
	
	unsigned int i, burst_len;
	of_switch_t* sw = port->attached_sw;
	struct rte_mbuf* mbuf;
	//dpdk_port_state_t* port_state = (dpdk_port_state_t*)port->platform_port_state;
	xdpd::gnu_linux::datapacketx86* pkt_x86 = pkt_state->pktx86;

	//Read a burst
	burst_len = rte_eth_rx_burst(port_id, 0, pkts_burst, IO_IFACE_MAX_PKT_BURST);

	//XXX: statistics

	//Process them 
	for(i=0;i<burst_len;++i){
		mbuf = pkts_burst[i];		

		if(unlikely(sw == NULL)){
			rte_pktmbuf_free(mbuf);
			continue;
		}

		//set mbuf pointer in the state so that it can be recovered afterwards when going
		//out from the pipeline
		pkt_state->mbuf = mbuf;

		//XXX: delete from here
		assert(mbuf->pkt.nb_segs == 1);

		//Init&classify	
		pkt_x86->init((uint8_t*)mbuf->buf_addr, mbuf->buf_len, sw, mbuf->pkt.in_port, 0, true, false);

		//Send to process
		of_process_packet_pipeline(sw, pkt);
	}	
}

inline void process_port_queue_tx(switch_port_t* port, unsigned int port_id, struct mbuf_table* queue, unsigned int queue_id){
	unsigned ret;

	if(queue->len == 0)
		return;

	//Send burst
	ret = rte_eth_tx_burst(port_id, queue_id, queue->m_table, queue->len);
	//XXX port_statistics[port].tx += ret;

	if (unlikely(ret < queue->len)) {
		//XXX port_statistics[port].dropped += (n - ret);
		do {
			rte_pktmbuf_free(queue->m_table[ret]);
		} while (++ret < queue->len);
	}

	//Reset queue size	
	queue->len = 0;
}

inline void tx_pkt(switch_port_t* port, unsigned int queue_id, datapacket_t* pkt){

	struct rte_mbuf* mbuf;
	struct mbuf_table* pkt_burst;
	unsigned int port_id, len;

	//Get mbuf pointer
	mbuf = ((dpdk_pkt_platform_state_t*)pkt->platform_state)->mbuf;
	port_id = ((dpdk_port_state_t*)port->platform_port_state)->port_id;

	if(unlikely(!mbuf)){
		assert(0);
		return;
	}
	
	//Recover core task
	core_tasks_t* tasks = &processing_cores[rte_lcore_id()];
	
	//Recover burst container
	pkt_burst = &tasks->all_ports[port_id].tx_queues[queue_id];	
	
	if(unlikely(!pkt_burst)){
		assert(0);
		return;
	}

	//Enqueue
	len = pkt_burst->len; 
	pkt_burst->m_table[len] = mbuf;
	len++;

	//If burst is full => trigger send
	if (unlikely(len == IO_IFACE_MAX_PKT_BURST)) {
		process_port_queue_tx(port, port_id, pkt_burst, queue_id);
		len = 0;
	}

	pkt_burst->len = len;
	return;
}

}// namespace xdpd::gnu_linux_dpdk 
}// namespace xdpd

#endif //_TX_RX_H_
