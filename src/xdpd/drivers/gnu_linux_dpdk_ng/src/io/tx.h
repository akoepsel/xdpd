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


namespace xdpd {
namespace gnu_linux_dpdk_ng {

//
// Packet TX
//

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

		//int socket_id = ps->socket_id;

		tx_events[0].flow_id = mbuf->hash.rss;
		tx_events[0].op = RTE_EVENT_OP_NEW;
		tx_events[0].sched_type = RTE_SCHED_TYPE_ATOMIC;
		tx_events[0].queue_id = event_queues[ps->socket_id][EVENT_QUEUE_TXCORES]; /* use queue-id for outgoing port's NUMA socket */
		tx_events[0].event_type = RTE_EVENT_TYPE_CPU;
		tx_events[0].sub_event_type = 0;
		tx_events[0].priority = RTE_EVENT_DEV_PRIORITY_NORMAL;
		tx_events[0].mbuf = mbuf;

		tx_events[0].mbuf->udata64 = (uint64_t)port_id;

		RTE_LOG(INFO, XDPD, "wk task %2u => eth-port-id: %u => event-port-id: %u, event-queue-id: %u, event[%u]\n",
				lcore_id, ps->port_id, ev_port_id, event_queues[ps->socket_id][EVENT_QUEUE_TXCORES], 0);

		int i = 0, nb_rx = 1;
		const int nb_tx = rte_event_enqueue_burst(eventdev_id, ev_port_id, tx_events, 1);
		if (nb_tx) {
			RTE_LOG(INFO, XDPD, "wk task %2u => event-port-id: %u, packets enqueued: %u\n",
					lcore_id, ev_port_id, nb_tx);
		}
		/* release mbufs not queued in event device */
		if (nb_tx != nb_rx) {
			for(i = nb_tx; i < nb_rx; i++) {
				RTE_LOG(WARNING, XDPD, "wk task %2u => event-port-id: %u, event-queue-id: %u, dropping mbuf[%u]\n",
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

		//int socket_id = rte_eth_dev_socket_id(ps->port_id);

		tx_events[0].flow_id = mbuf->hash.rss;
		tx_events[0].op = RTE_EVENT_OP_NEW;
		tx_events[0].sched_type = RTE_SCHED_TYPE_ATOMIC;
		tx_events[0].queue_id = task->tx_ev_queue_id[ps->socket_id]; /* use queue-id for outgoing port's NUMA socket */
		tx_events[0].event_type = RTE_EVENT_TYPE_CPU;
		tx_events[0].sub_event_type = 0;
		tx_events[0].priority = RTE_EVENT_DEV_PRIORITY_NORMAL;
		tx_events[0].mbuf = mbuf;

		tx_events[0].mbuf->udata64 = (uint64_t)port_id;

		RTE_LOG(INFO, XDPD, "wk task %2u => event-port-id: %u, event-queue-id: %u, event[%u] for eth-port: %u\n",
				lcore_id, task->ev_port_id, task->tx_ev_queue_id[ps->socket_id], 0, port_id);

		int i = 0, nb_rx = 1;
		const int nb_tx = rte_event_enqueue_burst(eventdev_id, task->ev_port_id, tx_events, 1);
		if (nb_tx) {
			RTE_LOG(INFO, XDPD, "wk task %2u => event-port-id: %u, packets enqueued: %u\n",
					lcore_id, task->ev_port_id, nb_tx);
		}
		/* release mbufs not queued in event device */
		if (nb_tx != nb_rx) {
			for(i = nb_tx; i < nb_rx; i++) {
				RTE_LOG(WARNING, XDPD, "wk task %2u => event-port-id: %u, event-queue-id: %u, dropping mbuf[%u]\n",
						lcore_id, task->ev_port_id, tx_events[i].queue_id, i);
				rte_pktmbuf_free(tx_events[i].mbuf);
			}
		}

	}

	//XDPD_DEBUG_VERBOSE(DRIVER_NAME"[io] Adding packet %p to queue %p (id: %u)\n", pkt, pkt_burst, lcore_id);

	return;

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
