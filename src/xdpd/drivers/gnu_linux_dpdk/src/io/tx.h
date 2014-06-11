/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _TX_H_
#define _TX_H_

#include "../config.h"
#include <rofl/common/utils/c_logger.h>
#include <rte_config.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_spinlock.h>
#include <rte_eal.h> 
#include <rte_mbuf.h> 
#include <rte_ethdev.h> 

#include <assert.h>
#include "bufferpool.h"
#include "dpdk_datapacket.h"

#include "port_state.h"
#include "iface_manager.h"
#include "../processing/processing.h"

namespace xdpd {
namespace gnu_linux_dpdk {

//
// Packet processing
//

inline void
transmit_port_queue_tx_burst(unsigned int port_id, unsigned int queue_id, struct rte_mbuf** burst){
	
	unsigned int ret, len;

	//Dequeue a burst from the TX ring	
	len = rte_ring_mc_dequeue_burst(port_tx_lcore_queue[port_id][queue_id], (void **)burst, IO_IFACE_MAX_PKT_BURST);      

	ROFL_DEBUG_VERBOSE(DRIVER_NAME"[io][%s(%u)] Trying to transmit burst on port queue_id %u of length %u\n",phy_port_mapping[port_id]->name,  port_id, queue_id, len);

	//Send burst
	ret = rte_eth_tx_burst(port_id, queue_id, burst, len);
	//XXX port_statistics[port].tx += ret;
	
	ROFL_DEBUG_VERBOSE(DRIVER_NAME"[io][%s(%u)] +++ Transmited %u pkts, on queue_id %u\n", phy_port_mapping[port_id]->name, port_id, ret, queue_id);

	if (unlikely(ret < len)) {
		//XXX port_statistics[port].dropped += (n - ret);
		do {
			rte_pktmbuf_free(burst[ret]);
		} while (++ret < len);
	}
}

inline void
flush_port_queue_tx_burst(switch_port_t* port, unsigned int port_id, struct mbuf_burst* queue, unsigned int queue_id){
	unsigned ret;

	if( queue->len == 0 || unlikely((port->up == false)) ){
		return;
	}

	ROFL_DEBUG_VERBOSE(DRIVER_NAME"[io][%s(%u)] Trying to flush burst(enqueue in lcore ring) on port queue_id %u of length: %u\n", port->name,  port_id, queue_id, queue->len);
		
	//Enqueue to the lcore (if it'd we us, we could probably call to transmit directly)
	ret = rte_ring_mp_enqueue_burst(port_tx_lcore_queue[port_id][queue_id], (void **)queue->burst, queue->len);
	//XXX port_statistics[port].tx += ret;
	
	ROFL_DEBUG_VERBOSE(DRIVER_NAME"[io][%s(%u)] --- Flushed %u pkts, on queue id %u\n", port->name, port_id, ret, queue_id);

	if (unlikely(ret < queue->len)) {
		//XXX port_statistics[port].dropped += (n - ret);
		do {
			rte_pktmbuf_free(queue->burst[ret]);
		} while (++ret < queue->len);
	}

	//Reset queue size	
	queue->len = 0;
}

inline void
tx_pkt(switch_port_t* port, unsigned int queue_id, datapacket_t* pkt){

	struct rte_mbuf* mbuf;
	struct mbuf_burst* pkt_burst;
	unsigned int port_id, len;

	//Get mbuf pointer
	mbuf = ((datapacket_dpdk_t*)pkt->platform_state)->mbuf;
	port_id = ((dpdk_port_state_t*)port->platform_port_state)->port_id;

#ifdef DEBUG
	if(unlikely(!mbuf)){
		assert(0);
		return;
	}
#endif
	
	//Recover core task
	core_tasks_t* tasks = &processing_core_tasks[rte_lcore_id()];
	
	//Recover burst container (cache)
	pkt_burst = &tasks->phy_ports[port_id].tx_queues_burst[queue_id];	

#if DEBUG	
	if(unlikely(!pkt_burst)){
		rte_pktmbuf_free(mbuf);
		assert(0);
		return;
	}
#endif

	ROFL_DEBUG_VERBOSE(DRIVER_NAME"[io] Adding packet %p to queue %p (id: %u)\n", pkt, pkt_burst, rte_lcore_id());

	//Enqueue
	len = pkt_burst->len; 
	pkt_burst->burst[len] = mbuf;
	len++;

	//If burst is full => trigger send
	if ( unlikely(!tasks->active) || unlikely(len == IO_IFACE_MAX_PKT_BURST)) { //If buffer is full or mgmt core
		pkt_burst->len = len;
		flush_port_queue_tx_burst(port, port_id, pkt_burst, queue_id);
		return;
	}

	pkt_burst->len = len;

	return;
}

/****************************************************************************
*						Funtions specific for PEX ports						*
*****************************************************************************/

/*
*	DPDK
*/

inline void
tx_pkt_dpdk_pex_port(switch_port_t* port, datapacket_t* pkt)
{
	assert(port->type == PORT_TYPE_PEX_DPDK);

	int ret;
	uint32_t tmp, next_tmp;
	uint64_t local_flush_time, cache_last_flush_time;
	pex_port_state_dpdk *port_state = (pex_port_state_dpdk_t*)port->platform_port_state;
	struct rte_mbuf* mbuf;
	
	//Get mbuf pointer
	mbuf = ((datapacket_dpdk_t*)pkt->platform_state)->mbuf;

	ret = rte_ring_mp_enqueue(port_state->to_pex_queue, (void *) mbuf);
	if( likely((ret == 0) || (ret == -EDQUOT)) )
	{
		//The packet has been enqueued
		
		//XXX port_statistics[port].tx++;
		
		//Increment the variable containing the number of pkts inserted
		//from the last sem_post
		do{
			tmp = port_state->counter_from_last_flush;
			if(tmp == (PKT_TO_PEX_THRESHOLD - 1))
				//Reset the counter
				next_tmp = 0;
			else
				next_tmp = tmp + 1;
		}while(__sync_bool_compare_and_swap(&(port_state->counter_from_last_flush),tmp,next_tmp) != false);
		
		if(tmp == (PKT_TO_PEX_THRESHOLD - 1))
		{
			//Notify that pkts are available
			sem_post(port_state->semaphore);
	
			//Store the current time
			local_flush_time = rte_rdtsc();

			do{
				cache_last_flush_time = port_state->last_flush_time; 
				if(port_state->last_flush_time > local_flush_time)
					break;
			}while(__sync_bool_compare_and_swap(&(port_state->last_flush_time),cache_last_flush_time,local_flush_time) != false);
		}
	}
	else
	{
		//The queue is full, and the pkt must be dropped
		
		//XXX port_statistics[port].dropped++
		
		rte_pktmbuf_free(mbuf);
	}
}

void inline
flush_dpdk_pex_port(switch_port_t *port)
{
	//IVANO - FIXME: this function is probably wrong
	return;

	uint64_t tmp_time;
	uint32_t tmp_counter_from_last_flush;
	
	assert(port != NULL);

	pex_port_state_dpdk *port_state = (pex_port_state_dpdk_t*)port->platform_port_state;

	//If there are pkts into the rte_ring, check if the timeout is expired
	tmp_time = port_state->last_flush_time;
	tmp_counter_from_last_flush = port_state->counter_from_last_flush;
	while( (tmp_counter_from_last_flush > 0) /* && ((rte_rdtsc() - tmp_time) > TIME_THRESHOLD) */)
	{
		if(__sync_bool_compare_and_swap(&(port_state->last_flush_time),tmp_time,rte_rdtsc()) == true)
		{
			sem_post(port_state->semaphore);
			break;
		}
		tmp_time = port_state->last_flush_time;
		tmp_counter_from_last_flush = port_state->counter_from_last_flush;
	}
}

/*
*	KNI
*/
inline void
flush_kni_pex_port(switch_port_t* port, unsigned int port_id, struct mbuf_burst* queue, unsigned int queue_id)
{
	unsigned ret;

	if( queue->len == 0 || unlikely((port->up == false)) ){
		return;
	}

	/*ROFL_DEBUG_VERBOSE*/ROFL_INFO(DRIVER_NAME"[io][%s(%u)] Trying to flush burst(enqueue in lcore ring) on port queue_id %u of length: %u\n", port->name,  port_id, queue_id, queue->len);
		
	//Enqueue to the lcore (if it'd we us, we could probably call to transmit directly)
	pex_port_state_kni *port_state = (pex_port_state_kni_t*)port->platform_port_state;
		
	assert(port_state != NULL);
	
	ret = rte_kni_tx_burst(port_state->kni, queue->burst, queue->len);
						
	//XXX port_statistics[port].tx += ret;
	
	ROFL_DEBUG_VERBOSE(DRIVER_NAME"[io][%s(%u)] --- Flushed %u pkts, on queue id %u\n", port->name, port_id, ret, queue_id);
	
	ROFL_INFO(DRIVER_NAME"[io][%s(%u)] --- Flushed %u pkts, on queue id %u\n", port->name, port_id, ret, queue_id);

	if (unlikely(ret < queue->len)) 
	{
		//XXX port_statistics[port].dropped += (n - ret);
		do {
			rte_pktmbuf_free(queue->burst[ret]);
		} while (++ret < queue->len);
	}

	//Reset queue size	
	queue->len = 0;
}

inline void 
tx_pkt_kni_pex_port(switch_port_t* port, unsigned int queue_id, datapacket_t* pkt)
{
	struct rte_mbuf* mbuf;
	struct mbuf_burst* pkt_burst;
	unsigned int port_id, len;

	//Get mbuf pointer
	mbuf = ((datapacket_dpdk_t*)pkt->platform_state)->mbuf;
	port_id = ((pex_port_state_kni_t*)port->platform_port_state)->pex_id;
	
	fprintf(stderr,"PKT TO BE SENT TO PORT %s\n",port->name);

#ifdef DEBUG
	if(unlikely(!mbuf)){
		assert(0);
		return;
	}
#endif
	
	//Recover core task
	core_tasks_t* tasks = &processing_core_tasks[rte_lcore_id()];
	
	//Recover burst container (cache)
	pkt_burst = &tasks->pex_ports[port_id].tx_queues_burst[queue_id];	

#if DEBUG	
	if(unlikely(!pkt_burst)){
		rte_pktmbuf_free(mbuf);
		assert(0);
		return;
	}
#endif

	ROFL_DEBUG_VERBOSE(DRIVER_NAME"[io] Adding packet %p to queue %p (id: %u)\n", pkt, pkt_burst, rte_lcore_id());

	//Enqueue
	len = pkt_burst->len; 
	pkt_burst->burst[len] = mbuf;
	len++;

	//If burst is full => trigger send
	if ( unlikely(!tasks->active) || unlikely(len == IO_IFACE_MAX_PKT_BURST)) { //If buffer is full or mgmt core
		pkt_burst->len = len;
		flush_kni_pex_port(port, port_id, pkt_burst, queue_id);
		return;
	}

	pkt_burst->len = len;

	return;
}


/****************************************************************************
*						Funtions specific for vlinks						*
*****************************************************************************/

void tx_pkt_vlink(switch_port_t* vlink, datapacket_t* pkt);

}// namespace xdpd::gnu_linux_dpdk 
}// namespace xdpd

#endif //_TX_H_
