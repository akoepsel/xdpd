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
	struct rte_event tx_event;
	uint16_t nb_tx;

	//Get mbuf pointer
	mbuf = ((datapacket_dpdk_t*)pkt->platform_state)->mbuf;
	assert(mbuf);
	port_id = ((dpdk_port_state_t*)port->platform_port_state)->port_id;

	lcore_id = rte_lcore_id();

	if ((lcore_id == LCORE_ID_ANY) || (lcores[lcore_id].is_master)) {

		/* use out_port_id from pipeline */
		rte_rwlock_read_lock(&port_list_rwlock);
		if ((port = port_list[port_id]) == NULL) {
			rte_rwlock_read_unlock(&port_list_rwlock);
			rte_pktmbuf_free(mbuf);
			return;
		}

		ps = (dpdk_port_state_t *)port->platform_port_state;

		tx_event.flow_id = mbuf->hash.rss;
		tx_event.op = RTE_EVENT_OP_NEW;
		tx_event.sched_type = RTE_SCHED_TYPE_PARALLEL;
		tx_event.queue_id = EVENT_QUEUE_CTRL_PLANE; /* use event queue leading to TX tasks on NUMA socket for outgoing port */
		tx_event.event_type = RTE_EVENT_TYPE_CPU;
		tx_event.sub_event_type = 0;
		tx_event.priority = RTE_EVENT_DEV_PRIORITY_NORMAL;
		tx_event.mbuf = mbuf;
		tx_event.mbuf->udata64 = (uint64_t)port_id;

		/* acquire rwlock for writing to eventdev port_id assigned to control plane threads */
		rte_rwlock_write_lock(&eventdev_port_ctrl_plane_rwlock);
		nb_tx = rte_event_enqueue_new_burst(ev_core_tasks[ps->socket_id].eventdev_id, EVENT_PORT_CTRL_PLANE, &tx_event, 1);
		rte_rwlock_write_unlock(&eventdev_port_ctrl_plane_rwlock);

		/* release mbufs not enqueued to event device */
		if (nb_tx < 1) {
			rte_pktmbuf_free(tx_event.mbuf);
		}

	} else
	if (lcores[lcore_id].is_wk_lcore) {

		//Recover worker task
		wk_core_task_t* task = &wk_core_tasks[lcore_id];

		if (unlikely(not task->available || not task->active)) {
			rte_pktmbuf_free(mbuf);
			return;
		}

		/* use outgoing port_id from pipeline */
		rte_rwlock_read_lock(&port_list_rwlock);
		if ((port = port_list[port_id]) == NULL) {
			rte_rwlock_read_unlock(&port_list_rwlock);
			rte_pktmbuf_free(mbuf);
			return;
		}

		/* returns number of flushed packets */
		nb_tx = rte_eth_tx_buffer(port_id, task->tx_queues[port_id].queue_id, task->tx_queues[port_id].tx_buffer, mbuf);

		/* update statistics */
		task->stats.tx_pkts+=nb_tx;
	}

	//XDPD_DEBUG_VERBOSE(DRIVER_NAME"[io] Adding packet %p to queue %p (id: %u)\n", pkt, pkt_burst, lcore_id);

	return;
}

}// namespace xdpd::gnu_linux_dpdk_ng
}// namespace xdpd

#endif //_TX_H_
