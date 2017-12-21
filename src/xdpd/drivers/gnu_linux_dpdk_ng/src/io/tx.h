/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _TX_H_
#define _TX_H_

#include "../config.h"
#include <utils/c_logger.h>
#include <rte_config.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_spinlock.h>
#include <rte_eal.h>
#include <rte_mbuf.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>

#include <assert.h>
#include "bufferpool.h"
#include "dpdk_datapacket.h"

#include "port_state.h"
#include "iface_manager.h"
#include "../processing/processing.h"

//#define TX_SHORTCUT 1

namespace xdpd {
namespace gnu_linux_dpdk_ng {

//
// Packet TX
//

inline void transmit_port_queue_tx_burst(tx_core_task_t *task, uint8_t port_id)
{
#if 0
	uint16_t ret;
	struct rte_mbuf **m_table;
	unsigned len;
	uint16_t queue_id;
	switch_port_t* port;

	queue_id = task->tx_queues[port_id].queue_id;
	m_table = (struct rte_mbuf **)task->tx_mbufs[port_id].burst;
	len = task->tx_mbufs[port_id].len;
	port = phy_port_mapping[port_id];

	RTE_LOG(INFO, XDPD, DRIVER_NAME "[io][%s(%u)] Trying to transmit burst on port queue_id %u of length %u\n",
		port->name, port_id, queue_id, len);

	//Send burst
	//rte_spinlock_lock(&spinlock_conf[port_id]);
	ret = rte_eth_tx_burst(port_id, queue_id, m_table, len);
	//rte_spinlock_unlock(&spinlock_conf[port_id]);

	if (ret)
		RTE_LOG(
		    INFO, XDPD,
		    "[io][%s(%u)] +++ Transmitted %u pkts, on queue_id %u\n",
		    phy_port_mapping[port_id]->name, port_id, ret, queue_id);

	if (unlikely(ret < len)) {
		//Increment errors
		port->stats.tx_dropped += ret;
		port->queues[queue_id].stats.overrun += ret;

		do {
			//Now release the mbuf
			rte_pktmbuf_free(m_table[ret]);
		} while (++ret < len);
	}
#endif
}

#if 0
inline void
flush_port_queue_tx_burst(switch_port_t* port, unsigned int port_id, struct mbuf_burst* queue, unsigned int queue_id){
	unsigned ret;

	if( queue->len == 0 || unlikely((port->up == false)) ){
		return;
	}

	XDPD_DEBUG_VERBOSE(DRIVER_NAME"[io][%s(%u)] Trying to flush burst(enqueue in lcore ring) on port queue_id %u of length: %u\n", port->name,  port_id, queue_id, queue->len);

	//Enqueue to the lcore (if it'd we us, we could probably call to transmit directly)
	ret = rte_ring_mp_enqueue_burst(port_tx_lcore_queue[port_id][queue_id], (void **)queue->burst, queue->len);

	XDPD_DEBUG_VERBOSE(DRIVER_NAME"[io][%s(%u)] --- Flushed %u pkts, on queue id %u\n", port->name, port_id, ret, queue_id);

	if (unlikely(ret < queue->len)) {
		//TODO increase error counters?
		do {
			rte_pktmbuf_free(queue->burst[ret]);
		} while (++ret < queue->len);
	}

	//Reset queue size
	queue->len = 0;
}
#endif

#ifdef TX_SHORTCUT
static inline int send_burst(core_tasks_t *qconf, uint16_t n, uint8_t port)
{
	struct rte_mbuf **m_table;
	int ret;
	uint16_t queueid;

	queueid = qconf->tx_queue_id[port];
	m_table = (struct rte_mbuf **)qconf->tx_mbufs[port].burst;

	ret = rte_eth_tx_burst(port, queueid, m_table, n);

	if (unlikely(ret < n)) {
		do {
			rte_pktmbuf_free(m_table[ret]);
		} while (++ret < n);
	}

	return 0;
}

inline void send_single_packet(struct rte_mbuf *m, uint8_t port)
{
	uint32_t lcore_id;
	uint16_t len;
	core_tasks_t *qconf;

	lcore_id = rte_lcore_id();

	qconf = &processing_core_tasks[lcore_id];
	len = qconf->tx_mbufs[port].len;
	qconf->tx_mbufs[port].burst[len] = m;
	len++;

	/* enough pkts to be sent */
	if (unlikely(len == IO_IFACE_MAX_PKT_BURST)) {
		send_burst(qconf, IO_IFACE_MAX_PKT_BURST, port);
		len = 0;
	}

	qconf->tx_mbufs[port].len = len;
}
#endif

inline void
tx_pkt(switch_port_t* port, unsigned int queue_id, datapacket_t* pkt){

	struct rte_mbuf* mbuf;
	dpdk_port_state_t* ps;
	unsigned int port_id, lcore_id;
	struct rte_event tx_events[PROC_ETH_TX_BURST_SIZE];

	//Get mbuf pointer
	mbuf = ((datapacket_dpdk_t*)pkt->platform_state)->mbuf;
	assert(mbuf);
	port_id = ((dpdk_port_state_t*)port->platform_port_state)->port_id;

	lcore_id = rte_lcore_id();

	if ((lcore_id == LCORE_ID_ANY) || (lcores[lcore_id].is_master)) {

		uint16_t ev_port_id = 0;

		/* use out_port_id from pipeline */
		rte_rwlock_read_lock(&port_list_rwlock);
		if ((port = port_list[port_id]) == NULL) {
			rte_rwlock_read_unlock(&port_list_rwlock);
			rte_pktmbuf_free(mbuf);
			return;
		}

		ps = (dpdk_port_state_t *)port->platform_port_state;

		int socket_id = ps->socket_id;

		tx_events[0].flow_id = mbuf->hash.rss;
		tx_events[0].op = RTE_EVENT_OP_NEW;
		tx_events[0].sched_type = RTE_SCHED_TYPE_ATOMIC;
		tx_events[0].queue_id = event_queues[socket_id][EVENT_QUEUE_TXCORES]; /* use queue-id for outgoing port's NUMA socket */
		tx_events[0].event_type = RTE_EVENT_TYPE_CPU;
		tx_events[0].sub_event_type = 0;
		tx_events[0].priority = RTE_EVENT_DEV_PRIORITY_NORMAL;
		tx_events[0].mbuf = mbuf;

		tx_events[0].mbuf->udata64 = (uint64_t)port_id;

		RTE_LOG(INFO, USER1, "wk task %2u => eth-port-id: %u => event-port-id: %u, event-queue-id: %u, event[%u] for eth-port: %u\n",
				lcore_id, ps->port_id, ev_port_id, event_queues[socket_id][EVENT_QUEUE_TXCORES], 0, port_id);

		int i = 0, nb_rx = 1;
		const int nb_tx = rte_event_enqueue_burst(eventdev_id, ev_port_id, tx_events, 1);
		if (nb_tx) {
			RTE_LOG(INFO, USER1, "wk task %2u => event-port-id: %u, packets enqueued: %u\n",
					lcore_id, ev_port_id, nb_tx);
		}
		/* release mbufs not queued in event device */
		if (nb_tx != nb_rx) {
			for(i = nb_tx; i < nb_rx; i++) {
				RTE_LOG(WARNING, USER1, "wk task %2u => event-port-id: %u, event-queue-id: %u, dropping mbuf[%u]\n",
						lcore_id, ev_port_id, tx_events[i].queue_id, i);
				rte_pktmbuf_free(tx_events[i].mbuf);
			}
		}

	} else
	if (lcores[lcore_id].is_wk_lcore) {


		//Recover worker task
		wk_core_task_t* task = &wk_core_tasks[lcore_id];

		if (unlikely(not task->available) || unlikely(not task->active)) {
			rte_pktmbuf_free(mbuf);
			return;
		}

		/* use out_port_id from pipeline */
		rte_rwlock_read_lock(&port_list_rwlock);
		if ((port = port_list[port_id]) == NULL) {
			rte_rwlock_read_unlock(&port_list_rwlock);
			rte_pktmbuf_free(mbuf);
			return;
		}

		ps = (dpdk_port_state_t *)port->platform_port_state;

		int socket_id = rte_eth_dev_socket_id(ps->port_id);

		tx_events[0].flow_id = mbuf->hash.rss;
		tx_events[0].op = RTE_EVENT_OP_NEW;
		tx_events[0].sched_type = RTE_SCHED_TYPE_ATOMIC;
		tx_events[0].queue_id = task->tx_ev_queue_id[socket_id]; /* use queue-id for outgoing port's NUMA socket */
		tx_events[0].event_type = RTE_EVENT_TYPE_CPU;
		tx_events[0].sub_event_type = 0;
		tx_events[0].priority = RTE_EVENT_DEV_PRIORITY_NORMAL;
		tx_events[0].mbuf = mbuf;

		tx_events[0].mbuf->udata64 = (uint64_t)port_id;

		RTE_LOG(INFO, USER1, "wk task %2u => event-port-id: %u, event-queue-id: %u, event[%u] for eth-port: %u\n",
				lcore_id, task->ev_port_id, task->tx_ev_queue_id[socket_id], 0, port_id);

		int i = 0, nb_rx = 1;
		const int nb_tx = rte_event_enqueue_burst(eventdev_id, task->ev_port_id, tx_events, 1);
		if (nb_tx) {
			RTE_LOG(INFO, USER1, "wk task %2u => event-port-id: %u, packets enqueued: %u\n",
					lcore_id, task->ev_port_id, nb_tx);
		}
		/* release mbufs not queued in event device */
		if (nb_tx != nb_rx) {
			for(i = nb_tx; i < nb_rx; i++) {
				RTE_LOG(WARNING, USER1, "wk task %2u => event-port-id: %u, event-queue-id: %u, dropping mbuf[%u]\n",
						lcore_id, task->ev_port_id, tx_events[i].queue_id, i);
				rte_pktmbuf_free(tx_events[i].mbuf);
			}
		}

	}

	//XDPD_DEBUG_VERBOSE(DRIVER_NAME"[io] Adding packet %p to queue %p (id: %u)\n", pkt, pkt_burst, lcore_id);

	return;

}

#if 0
inline void
tx_pkt(switch_port_t* port, unsigned int queue_id, datapacket_t* pkt){

	struct rte_mbuf* mbuf;
	struct mbuf_burst* pkt_burst;
	unsigned int port_id, len, rte_lcore;

	//Get mbuf pointer
	mbuf = ((datapacket_dpdk_t*)pkt->platform_state)->mbuf;
	assert(mbuf);
	port_id = ((dpdk_port_state_t*)port->platform_port_state)->port_id;

	rte_lcore = rte_lcore_id();
	if (rte_lcore == 0xffffffff) rte_lcore=0;

	//Recover core task
	tx_core_task_t* tasks = &tx_core_tasks[rte_lcore];

	//Recover burst container (cache)
	pkt_burst = &tasks->tx_mbufs[port_id];

	XDPD_DEBUG_VERBOSE(DRIVER_NAME"[io] Adding packet %p to queue %p (id: %u)\n", pkt, pkt_burst, rte_lcore);

	//Enqueue
	len = pkt_burst->len;
	pkt_burst->burst[len] = mbuf;
	len++;

	//If burst is full => trigger send
	if ( unlikely(!tasks->active) || unlikely(len == IO_IFACE_MAX_PKT_BURST)) { //If buffer is full or mgmt core
		pkt_burst->len = len;
		transmit_port_queue_tx_burst(tasks, port_id);
		len = 0;
	}

	pkt_burst->len = len;

	return;

}
#endif

//
// Specific NF port functions
//
#ifdef GNU_LINUX_DPDK_ENABLE_NF

/**
* Shmem port
*/
void inline
flush_shmem_nf_port(switch_port_t* port, rte_ring* queue, struct mbuf_burst* burst){
#if 0
	unsigned ret;
#ifdef ENABLE_DPDK_SECONDARY_SEMAPHORE
	uint32_t tmp, next_tmp;
#endif
	if( burst->len == 0 || unlikely((port->up == false)) ){
		return;
	}

	XDPD_DEBUG_VERBOSE(DRIVER_NAME"[io][shmem][%s] Trying to flush burst(enqueue in lcore ring) of length: %u\n", port->name,  burst->len);

	//Enqueue to the shmem ring
	ret = rte_ring_mp_enqueue_burst(queue, (void **)burst->burst, burst->len, NULL);

	XDPD_DEBUG_VERBOSE(DRIVER_NAME"[io][shmem][%s] --- Flushed %u pkts\n", port->name, ret);

#ifdef ENABLE_DPDK_SECONDARY_SEMAPHORE
	unsigned int ret_cpy = ret;
#endif

	if (unlikely(ret < burst->len)) {
		//TODO increase error counters?
		do {
			rte_pktmbuf_free(burst->burst[ret]);
		} while (++ret < burst->len);
	}

#ifdef ENABLE_DPDK_SECONDARY_SEMAPHORE
	dpdk_shmem_port_state *port_state = (dpdk_shmem_port_state_t*)port->platform_port_state;
	if( likely(ret_cpy >0)){
		unsigned int i;

		//The packet has been enqueued

		//XXX port_statistics[port].tx++;

		//Increment the variable containing the number of pkts inserted
		//from the last sem_post
		do{
			tmp = port_state->counter_from_last_flush;
			next_tmp = (tmp + ret_cpy) % PKT_TO_NF_THRESHOLD;
		}while(__sync_bool_compare_and_swap(&(port_state->counter_from_last_flush),tmp,next_tmp) == false);

		//Notify that pkts are available
		for(i=0;i<ret_cpy;++i)
			sem_post(port_state->semaphore);
	}
#endif

	//Reset queue size
	burst->len = 0;
#endif
}

inline void
tx_pkt_shmem_nf_port(switch_port_t* port, datapacket_t* pkt)
{
#if 0
	struct mbuf_burst* pkt_burst;
	unsigned int len, rte_lcore;

	dpdk_shmem_port_state *port_state = (dpdk_shmem_port_state_t*)port->platform_port_state;
	struct rte_mbuf* mbuf;

	//Get mbuf pointer
	mbuf = ((datapacket_dpdk_t*)pkt->platform_state)->mbuf;

	rte_lcore = rte_lcore_id();
        if (rte_lcore == 0xffffffff) rte_lcore=0;

        //Recover core task
        wk_core_task_t* tasks = &wk_core_tasks[rte_lcore];
        
	//Recover burst container (cache)
	pkt_burst = &tasks->tx_mbufs[port_state->state.port_id];

	assert(pkt_burst);

	XDPD_DEBUG_VERBOSE(DRIVER_NAME"[io][shmem] Adding packet %p to queue %p (id: %u)\n", pkt, pkt_burst, rte_lcore);

	//Enqueue
	len = pkt_burst->len;
	pkt_burst->burst[len] = mbuf;
	len++;

	//If burst is full => trigger send
	if ( unlikely(!tasks->active) || unlikely(len == IO_IFACE_MAX_PKT_BURST)) { //If buffer is full or mgmt core
		pkt_burst->len = len;
		flush_shmem_nf_port(port, port_state->to_nf_queue, pkt_burst);
		return;
	}

	pkt_burst->len = len;
#endif
}

/**
* KNI
*/
inline void
transmit_kni_nf_port_burst(tx_core_task_t *task, uint8_t port_id)
{
#if 0
	uint16_t ret;
	struct rte_mbuf **m_table;
	unsigned len;
	switch_port_t* port;
	dpdk_kni_port_state *port_state;

	m_table = (struct rte_mbuf **)task->tx_mbufs[port_id].burst;
	len = task->tx_mbufs[port_id].len;
	port = nf_port_mapping[port_id - nb_phy_ports];

	assert(port);

	RTE_LOG(INFO, XDPD, DRIVER_NAME "[io][kni] Trying to transmit burst on KNI port %s of length %u\n", port->name,
		len);

	//Send burst
	port_state = (dpdk_kni_port_state_t*)port->platform_port_state;

	//rte_spinlock_lock(&spinlock_conf[port_id]);
	ret = rte_kni_tx_burst(port_state->kni, m_table, len);
	//rte_spinlock_unlock(&spinlock_conf[port_id]);

	//XXX port_statistics[port].tx += ret;
	if (ret > 0)
		RTE_LOG(INFO, XDPD, DRIVER_NAME "[io][kni] Transmited %u pkts, on port %s\n", ret,
			port->name);

	if (unlikely(ret < len)) {
		//XXX port_statistics[port].dropped += (n - ret);
		do {
			rte_pktmbuf_free(m_table[ret]);
		} while (++ret < len);
	}
#endif
}

inline void
flush_kni_nf_port_burst(switch_port_t* port, unsigned int port_id, struct mbuf_burst* queue)
{
#if 0
	unsigned ret;

	if( queue->len == 0 || unlikely((port->up == false)) ){
		return;
	}

	assert((dpdk_kni_port_state_t*)port->platform_port_state != NULL);

	XDPD_DEBUG_VERBOSE(DRIVER_NAME"[io][kni] Trying to flush burst(enqueue in lcore ring) on KNI port %s\n", port->name, queue->len);


	ret = rte_ring_mp_enqueue_burst(port_tx_nf_lcore_queue[port_id], (void **)queue->burst, queue->len, NULL);

	//XXX port_statistics[port].tx += ret;

	XDPD_DEBUG_VERBOSE(DRIVER_NAME"[io][kni] --- Flushed %u pkts, on KNI port %s\n", port_id, ret, port->name);

	if (unlikely(ret < queue->len))
	{
		//XXX port_statistics[port].dropped += (n - ret);
		do {
			rte_pktmbuf_free(queue->burst[ret]);
		} while (++ret < queue->len);
	}

	//Reset queue size
	queue->len = 0;
#endif
}

inline void
tx_pkt_kni_nf_port(switch_port_t* port, datapacket_t* pkt)
{
#if 0
	struct rte_mbuf* mbuf;
	struct mbuf_burst* pkt_burst;
	unsigned int port_id, len, rte_lcore;

	//Get mbuf pointer
	mbuf = ((datapacket_dpdk_t*)pkt->platform_state)->mbuf;
	port_id = ((dpdk_port_state_t*)port->platform_port_state)->port_id;

	assert(mbuf);

	rte_lcore = rte_lcore_id();
	if (rte_lcore == 0xffffffff) rte_lcore=0;

	//Recover core task
	tx_core_task_t* tasks = &tx_core_tasks[rte_lcore];

	//Recover burst container (cache)
	pkt_burst = &tasks->tx_mbufs[port_id];
	assert(pkt_burst);

	XDPD_DEBUG_VERBOSE(DRIVER_NAME"[io][kni] Adding packet %p to queue %p (id: %u)\n", pkt, pkt_burst, rte_lcore);

	//Enqueue
	len = pkt_burst->len;
	pkt_burst->burst[len] = mbuf;
	len++;

	//If burst is full => trigger send
	if ( unlikely(!tasks->active) || unlikely(len == IO_IFACE_MAX_PKT_BURST))
	{
		//If buffer is full or mgmt core
		pkt_burst->len = len;
		transmit_kni_nf_port_burst(tasks, port_id);
		len = 0;
	}

	pkt_burst->len = len;

	return;
#endif
}

#endif //GNU_LINUX_DPDK_ENABLE_NF

inline void process_port_tx(tx_core_task_t *task, uint8_t port_id)
{
	switch_port_t* port;

	port = port_list[port_id];
	assert(port_id == ((dpdk_port_state_t*)port->platform_port_state)->port_id);

	switch(port->type){
	case PORT_TYPE_PHYSICAL:
		transmit_port_queue_tx_burst(task, port_id);
		break;
#ifdef GNU_LINUX_DPDK_ENABLE_NF
	case PORT_TYPE_NF_EXTERNAL:
		transmit_kni_nf_port_burst(task, port_id);
		break;
	case PORT_TYPE_NF_SHMEM:
		assert(0 && "missing implementation");
		break;
#endif
	default:
		assert(0 && "invalid branch");
		break;
	}
}

//
// vlink specific functions
//

/**
* Transmit a packet through a vlink
*/
void tx_pkt_vlink(switch_port_t* vlink, datapacket_t* pkt);

}// namespace xdpd::gnu_linux_dpdk_ng
}// namespace xdpd

#endif //_TX_H_
