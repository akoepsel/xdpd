/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _DPDK_DATAPACKET_H_
#define _DPDK_DATAPACKET_H_

#include <rte_common.h> 
#include <rte_eal.h> 
#include <rte_mbuf.h> 

#include "datapacketx86.h"

namespace xdpd {
namespace gnu_linux_dpdk {


/*
* Binds datapacket with dpdk's mbuf
*/
typedef struct dpdk_pkt_platform_state{
	struct rte_mbuf* mbuf;
	xdpd::gnu_linux::datapacketx86* pkt_x86;
}dpdk_pkt_platform_state_t;

}// namespace xdpd::gnu_linux_dpdk 
}// namespace xdpd


#endif //_DPDK_DATAPACKET_H_
