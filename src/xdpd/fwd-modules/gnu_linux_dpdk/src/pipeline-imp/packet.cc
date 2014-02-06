#define __STDC_LIMIT_MACROS
#include <inttypes.h>
#include <rofl/datapath/pipeline/physical_switch.h>
#include <rofl/datapath/pipeline/common/datapacket.h>
#include <rofl/datapath/pipeline/openflow/of_switch.h>
#include <rofl/datapath/pipeline/platform/packet.h>
#include <rofl/datapath/afa/openflow/openflow1x/of1x_cmm.h>
#include <rofl/datapath/pipeline/common/ipv6_exthdr.h>
#include <rofl/common/utils/c_logger.h>

#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "../config.h"
#include <rte_config.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_spinlock.h>
#include <rte_ring.h>
#include <rte_errno.h>


#include "../io/bufferpool.h"
#include "../io/dpdk_datapacket.h"
#include "../io/tx_rx.h"

using namespace xdpd::gnu_linux;
using namespace xdpd::gnu_linux_dpdk;

//MBUF pool
extern struct rte_mempool* pool_direct;

/*
* ROFL-Pipeline packet mangling platform API implementation
*/

//Getters
uint32_t
platform_packet_get_size_bytes(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	return get_buffer_length_dpdk(pack);
}

uint32_t
platform_packet_get_port_in(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if (NULL == pack) return 0;
	return pack->in_port;
}

uint32_t
platform_packet_get_phy_port_in(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if (NULL == pack) return 0;
	return pack->in_phy_port;
}

uint64_t
platform_packet_get_eth_dst(datapacket_t * const pkt)
{

	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
        
	if ((NULL == pack) || (NULL == get_ether_hdr(pack->headers,0))) return 0;

	return get_ether_dl_dst(get_ether_hdr(pack->headers,0));
}

uint64_t
platform_packet_get_eth_src(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
        
	if ((NULL == pack) || (NULL == get_ether_hdr(pack->headers,0))) return 0;
        
	return get_ether_dl_src(get_ether_hdr(pack->headers,0));
}


uint16_t
platform_packet_get_eth_type(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_ether_hdr(pack->headers,0))) return 0;
	
	if (get_vlan_hdr(pack->headers,-1) != NULL)
		return get_vlan_type(get_vlan_hdr(pack->headers,-1));
	
	return get_ether_type(get_ether_hdr(pack->headers,0));
}

bool
platform_packet_has_vlan(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if (NULL == pack)
		return false;
	return (NULL != get_vlan_hdr(pack->headers,0));
}
uint16_t
platform_packet_get_vlan_vid(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_vlan_hdr(pack->headers,0))) return 0;
	
	return get_vlan_id(get_vlan_hdr(pack->headers,0))&0xFFF;
}

uint8_t
platform_packet_get_vlan_pcp(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_vlan_hdr(pack->headers,0))) return 0;
	return get_vlan_pcp(get_vlan_hdr(pack->headers,0))&0x07;
}

uint16_t
platform_packet_get_arp_opcode(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_arpv4_hdr(pack->headers,0))) return 0;
	return get_arpv4_opcode(get_arpv4_hdr(pack->headers,0));
}

uint64_t
platform_packet_get_arp_sha(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_arpv4_hdr(pack->headers,0))) return 0;
	return get_arpv4_dl_src(get_arpv4_hdr(pack->headers,0));
}

uint32_t
platform_packet_get_arp_spa(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_arpv4_hdr(pack->headers,0))) return 0;
	return get_arpv4_ip_src(get_arpv4_hdr(pack->headers,0));
}

uint64_t
platform_packet_get_arp_tha(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_arpv4_hdr(pack->headers,0))) return 0;
	return get_arpv4_dl_dst(get_arpv4_hdr(pack->headers,0));
}

uint32_t
platform_packet_get_arp_tpa(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_arpv4_hdr(pack->headers,0))) return 0;
	return get_arpv4_ip_dst(get_arpv4_hdr(pack->headers,0));
}

uint8_t
platform_packet_get_ip_ecn(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if (NULL == pack) return 0;
	if (NULL != get_ipv4_hdr(pack->headers,0))
		return get_ipv4_ecn(get_ipv4_hdr(pack->headers,0))&0xFF;
	if (NULL != get_ipv6_hdr(pack->headers,0))
		return get_ipv6_ecn(get_ipv6_hdr(pack->headers,0));
	return 0;
}

uint8_t
platform_packet_get_ip_dscp(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if (NULL == pack) return 0;
	if (NULL != get_ipv4_hdr(pack->headers,0))
		return get_ipv4_dscp(get_ipv4_hdr(pack->headers,0))&0xFF;
	if (NULL != get_ipv6_hdr(pack->headers,0))
		return get_ipv6_dscp(get_ipv6_hdr(pack->headers,0));
	return 0;
}

uint8_t
platform_packet_get_ip_proto(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if (NULL == pack) return 0;
	if (NULL != get_ipv4_hdr(pack->headers,0))
		return get_ipv4_proto(get_ipv4_hdr(pack->headers,0))&0xFF;
	if (NULL != get_ipv6_hdr(pack->headers,0))
		return get_ipv6_next_header(get_ipv6_hdr(pack->headers,0))&0xFF;
	return 0;
}

uint32_t
platform_packet_get_ipv4_src(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_ipv4_hdr(pack->headers,0))) return 0;

	return get_ipv4_src(get_ipv4_hdr(pack->headers,0));
}

uint32_t
platform_packet_get_ipv4_dst(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_ipv4_hdr(pack->headers,0))) return 0;

	return get_ipv4_dst(get_ipv4_hdr(pack->headers,0));
}

uint16_t
platform_packet_get_tcp_dst(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_tcp_hdr(pack->headers,0))) return 0;
	return get_tcp_dport(get_tcp_hdr(pack->headers,0));
}

uint16_t
platform_packet_get_tcp_src(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_tcp_hdr(pack->headers,0))) return 0;
	return get_tcp_sport(get_tcp_hdr(pack->headers,0));
}

uint16_t
platform_packet_get_udp_dst(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_udp_hdr(pack->headers,0))) return 0;
	return get_udp_dport(get_udp_hdr(pack->headers,0));
}

uint16_t
platform_packet_get_udp_src(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_udp_hdr(pack->headers,0))) return 0;
	return get_udp_sport(get_udp_hdr(pack->headers,0));
}

uint16_t platform_packet_get_sctp_dst(datapacket_t *const pkt){
	//TODO: add implementation when supported
	return 0x0;
}
uint16_t platform_packet_get_sctp_src(datapacket_t *const pkt){
	//TODO: add implementation when supported
	return 0x0;
}


uint8_t
platform_packet_get_icmpv4_type(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_icmpv4_hdr(pack->headers,0))) return 0;
	return get_icmpv4_type(get_icmpv4_hdr(pack->headers,0));
}

uint8_t
platform_packet_get_icmpv4_code(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_icmpv4_hdr(pack->headers,0))) return 0;
	return get_icmpv4_code(get_icmpv4_hdr(pack->headers,0));
}

uint128__t
platform_packet_get_ipv6_src(datapacket_t * const pkt)
{
	uint128__t ipv6_src = {{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}}; //memset 0?
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if((NULL==pkt) || (NULL == get_ipv6_hdr(pack->headers,0))) return ipv6_src;
	ipv6_src = get_ipv6_src(get_ipv6_hdr(pack->headers,0));
	return ipv6_src;
}

uint128__t
platform_packet_get_ipv6_dst(datapacket_t * const pkt)
{
	uint128__t ipv6_dst = {{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}}; //memset 0?
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if((NULL==pkt) || (NULL == get_ipv6_hdr(pack->headers,0))) return ipv6_dst;
	ipv6_dst = get_ipv6_dst(get_ipv6_hdr(pack->headers,0));
	return ipv6_dst;
}

uint64_t
platform_packet_get_ipv6_flabel(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if((NULL==pkt) || (NULL == get_ipv6_hdr(pack->headers,0))) return 0;
	return get_ipv6_flow_label(get_ipv6_hdr(pack->headers,0));
}

uint128__t
platform_packet_get_ipv6_nd_target(datapacket_t * const pkt)
{
	uint128__t ipv6_nd_target = {{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}}; //memset 0?
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if((NULL==pkt) || (NULL == get_icmpv6_hdr(pack->headers,0))) return ipv6_nd_target;
	ipv6_nd_target = get_icmpv6_neighbor_taddr(get_icmpv6_hdr(pack->headers,0));
	return ipv6_nd_target;
}

uint64_t
platform_packet_get_ipv6_nd_sll(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if((NULL==pkt) || (NULL == get_icmpv6_hdr(pack->headers,0))) return 0;
	try{
		return get_icmpv6_ll_saddr(get_icmpv6_hdr(pack->headers,0));
	}catch(...){
		return 0;
	}
}

uint64_t
platform_packet_get_ipv6_nd_tll(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if((NULL==pkt) || (NULL == get_icmpv6_hdr(pack->headers,0))) return 0;
	try{
		return get_icmpv6_ll_taddr(get_icmpv6_hdr(pack->headers,0));
	}catch(...)	{
		return 0;
	}
}

uint16_t
platform_packet_get_ipv6_exthdr(datapacket_t * const pkt)
{
	uint64_t mask=0x0;
	//TODO EXTENSION HEADERS NOT YET IMPLEMENTED
	return mask;
}

uint8_t
platform_packet_get_icmpv6_type(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if((NULL==pkt) || (NULL == get_icmpv6_hdr(pack->headers,0))) return 0;
	return get_icmpv6_type(get_icmpv6_hdr(pack->headers,0));
}

uint8_t
platform_packet_get_icmpv6_code(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if((NULL==pkt) || (NULL == get_icmpv6_hdr(pack->headers,0))) return 0;
	return get_icmpv6_code(get_icmpv6_hdr(pack->headers,0));
}

uint32_t
platform_packet_get_mpls_label(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_mpls_hdr(pack->headers,0))) return 0;
	return get_mpls_label(get_mpls_hdr(pack->headers,0))&0x000FFFFF;
}

uint8_t
platform_packet_get_mpls_tc(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_mpls_hdr(pack->headers,0))) return 0;
	return get_mpls_tc(get_mpls_hdr(pack->headers,0))&0x07;
}

bool
platform_packet_get_mpls_bos(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_mpls_hdr(pack->headers,0))) return 0;
	return get_mpls_bos(get_mpls_hdr(pack->headers,0))&0x01;
}

uint32_t platform_packet_get_pbb_isid(datapacket_t *const pkt){
	//TODO: add implementation when supported
	return 0x0;
}

//Tunnel id
uint64_t platform_packet_get_tunnel_id(datapacket_t *const pkt){
	//TODO: add implementation when supported
	return 0x0ULL;
}


uint8_t
platform_packet_get_pppoe_code(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_pppoe_hdr(pack->headers,0))) return 0;
	return get_pppoe_code(get_pppoe_hdr(pack->headers,0));
}

uint8_t
platform_packet_get_pppoe_type(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_pppoe_hdr(pack->headers,0))) return 0;
	return get_pppoe_type(get_pppoe_hdr(pack->headers,0))&0x0F;
}

uint16_t
platform_packet_get_pppoe_sid(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_pppoe_hdr(pack->headers,0))) return 0;
	return get_pppoe_sessid(get_pppoe_hdr(pack->headers,0));
}

uint16_t
platform_packet_get_ppp_proto(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_ppp_hdr(pack->headers,0))) return 0;
	return get_ppp_prot(get_ppp_hdr(pack->headers,0));
}

uint8_t
platform_packet_get_gtp_msg_type(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_gtpu_hdr(pack->headers,0))) return 0;
	return get_gtpu_msg_type(get_gtpu_hdr(pack->headers,0));
}

uint32_t
platform_packet_get_gtp_teid(datapacket_t * const pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_gtpu_hdr(pack->headers,0))) return 0;
	return get_gtpu_teid(get_gtpu_hdr(pack->headers,0));
}


//Actions
void
platform_packet_copy_ttl_in(datapacket_t* pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if (NULL == pack) return;
	// TODO
}

void
platform_packet_pop_vlan(datapacket_t* pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if (NULL == pack) return;
	pop_vlan(pkt, pack->headers);
}

void
platform_packet_pop_mpls(datapacket_t* pkt, uint16_t ether_type)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if (NULL == pack) return;
	pop_mpls(pkt, pack->headers, ether_type);
}

void
platform_packet_pop_pppoe(datapacket_t* pkt, uint16_t ether_type)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if (NULL == pack) return;
	pop_pppoe(pkt, pack->headers, ether_type);
}

void
platform_packet_push_pppoe(datapacket_t* pkt, uint16_t ether_type)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if (NULL == pack) return;
	push_pppoe(pkt, pack->headers, ether_type);
}

void
platform_packet_push_mpls(datapacket_t* pkt, uint16_t ether_type)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if (NULL == pack) return;
	push_mpls(pkt, pack->headers, ether_type);
}

void
platform_packet_push_vlan(datapacket_t* pkt, uint16_t ether_type)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if (NULL == pack) return;
	push_vlan(pkt, pack->headers, ether_type);
}

void
platform_packet_copy_ttl_out(datapacket_t* pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if (NULL == pack) return;
	// TODO
}

void
platform_packet_dec_nw_ttl(datapacket_t* pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if (NULL == pack)
		return;
	if(NULL != get_ipv4_hdr(pack->headers,0)){
		dec_ipv4_ttl(get_ipv4_hdr(pack->headers,0));
		pack->ipv4_recalc_checksum = true;
	}
	if(NULL != get_ipv6_hdr(pack->headers,0)){
		dec_ipv6_hop_limit(get_ipv6_hdr(pack->headers,0));
	}
}

void
platform_packet_dec_mpls_ttl(datapacket_t* pkt)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_mpls_hdr(pack->headers,0))) return;
	dec_mpls_ttl(get_mpls_hdr(pack->headers,0));
}

void
platform_packet_set_mpls_ttl(datapacket_t* pkt, uint8_t new_ttl)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_mpls_hdr(pack->headers,0))) return;
	set_mpls_ttl(get_mpls_hdr(pack->headers,0), new_ttl);
}

void
platform_packet_set_nw_ttl(datapacket_t* pkt, uint8_t new_ttl)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if (NULL == pack) return;
	if (NULL != get_ipv4_hdr(pack->headers,0)){
		set_ipv4_ttl(get_ipv4_hdr(pack->headers,0), new_ttl);
		pack->ipv4_recalc_checksum = true;
	}
	if (NULL != get_ipv6_hdr(pack->headers,0)){
		set_ipv6_hop_limit(get_ipv6_hdr(pack->headers,0), new_ttl);
	}
}

void
platform_packet_set_queue(datapacket_t* pkt, uint32_t queue)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if (NULL == pack) return;

	pack->output_queue = queue;	
}

void
platform_packet_set_eth_dst(datapacket_t* pkt, uint64_t eth_dst)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_ether_hdr(pack->headers,0))) return;
	set_ether_dl_dst(get_ether_hdr(pack->headers,0), eth_dst);
}

void
platform_packet_set_eth_src(datapacket_t* pkt, uint64_t eth_src)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_ether_hdr(pack->headers,0))) return;
	set_ether_dl_src(get_ether_hdr(pack->headers,0), eth_src);
}

void
platform_packet_set_eth_type(datapacket_t* pkt, uint16_t eth_type)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_ether_hdr(pack->headers,0))) return;
	set_ether_type(get_ether_hdr(pack->headers,0), eth_type);
}

void
platform_packet_set_vlan_vid(datapacket_t* pkt, uint16_t vlan_vid)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_vlan_hdr(pack->headers,0))) return;
	set_vlan_id(get_vlan_hdr(pack->headers,0), vlan_vid);
}

void
platform_packet_set_vlan_pcp(datapacket_t* pkt, uint8_t vlan_pcp)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_vlan_hdr(pack->headers,0))) return;
	set_vlan_pcp(get_vlan_hdr(pack->headers,0), vlan_pcp);
}

void
platform_packet_set_arp_opcode(datapacket_t* pkt, uint16_t arp_opcode)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_arpv4_hdr(pack->headers,0))) return;
	set_arpv4_opcode(get_arpv4_hdr(pack->headers,0), arp_opcode);
}

void
platform_packet_set_arp_sha(datapacket_t* pkt, uint64_t arp_sha)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_arpv4_hdr(pack->headers,0))) return;
	set_arpv4_dl_src(get_arpv4_hdr(pack->headers,0), arp_sha);
}

void
platform_packet_set_arp_spa(datapacket_t* pkt, uint32_t arp_spa)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_arpv4_hdr(pack->headers,0))) return;
	set_arpv4_ip_src(get_arpv4_hdr(pack->headers,0), arp_spa);
}

void
platform_packet_set_arp_tha(datapacket_t* pkt, uint64_t arp_tha)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_arpv4_hdr(pack->headers,0))) return;
	set_arpv4_dl_dst(get_arpv4_hdr(pack->headers,0), arp_tha);
}

void
platform_packet_set_arp_tpa(datapacket_t* pkt, uint32_t arp_tpa)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_arpv4_hdr(pack->headers,0))) return;
	set_arpv4_ip_dst(get_arpv4_hdr(pack->headers,0), arp_tpa);
}

void
platform_packet_set_ip_dscp(datapacket_t* pkt, uint8_t ip_dscp)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if (NULL == pack) return;
	if (NULL != get_ipv4_hdr(pack->headers,0)) {
		set_ipv4_dscp(get_ipv4_hdr(pack->headers,0), ip_dscp);
		pack->ipv4_recalc_checksum = true;
	}
	if (NULL != get_ipv6_hdr(pack->headers,0)) {
		set_ipv6_dscp(get_ipv6_hdr(pack->headers,0), ip_dscp);
	}
}

void
platform_packet_set_ip_ecn(datapacket_t* pkt, uint8_t ip_ecn)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if (NULL == pack) return;
	if (NULL != get_ipv4_hdr(pack->headers,0)){
		set_ipv4_ecn(get_ipv4_hdr(pack->headers,0), ip_ecn);
		pack->ipv4_recalc_checksum = true;
	}
	if (NULL != get_ipv6_hdr(pack->headers,0)){
		set_ipv6_ecn(get_ipv6_hdr(pack->headers,0), ip_ecn);
	}
}

void
platform_packet_set_ip_proto(datapacket_t* pkt, uint8_t ip_proto)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if (NULL == pack) return;
	if (NULL != get_ipv4_hdr(pack->headers,0)) {
		set_ipv4_proto(get_ipv4_hdr(pack->headers,0), ip_proto);
		pack->ipv4_recalc_checksum = true;
	}
	if (NULL != get_ipv6_hdr(pack->headers,0)) {
		set_ipv6_next_header(get_ipv6_hdr(pack->headers,0), ip_proto);
	}
}

void
platform_packet_set_ipv4_src(datapacket_t* pkt, uint32_t ip_src)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_ipv4_hdr(pack->headers,0))) return;
	set_ipv4_src(get_ipv4_hdr(pack->headers,0), ip_src);
	pack->ipv4_recalc_checksum = true;
	pack->tcp_recalc_checksum = true;
	pack->udp_recalc_checksum = true;
}

void
platform_packet_set_ipv4_dst(datapacket_t* pkt, uint32_t ip_dst)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_ipv4_hdr(pack->headers,0))) return;
	set_ipv4_dst(get_ipv4_hdr(pack->headers,0), ip_dst);
	pack->ipv4_recalc_checksum = true;
	pack->tcp_recalc_checksum = true;
	pack->udp_recalc_checksum = true;
}

void
platform_packet_set_ipv6_src(datapacket_t* pkt, uint128__t ipv6_src)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_ipv6_hdr(pack->headers,0))) return;
	set_ipv6_src(get_ipv6_hdr(pack->headers,0), ipv6_src);
	pack->tcp_recalc_checksum = true;
	pack->udp_recalc_checksum = true;
}

void
platform_packet_set_ipv6_dst(datapacket_t* pkt, uint128__t ipv6_dst)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_ipv6_hdr(pack->headers,0))) return;
	set_ipv6_dst(get_ipv6_hdr(pack->headers,0), ipv6_dst);
	pack->tcp_recalc_checksum = true;
	pack->udp_recalc_checksum = true;
}

void
platform_packet_set_ipv6_flabel(datapacket_t* pkt, uint64_t ipv6_flabel)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_ipv6_hdr(pack->headers,0))) return;
	set_ipv6_flow_label(get_ipv6_hdr(pack->headers,0), ipv6_flabel);
}

void
platform_packet_set_ipv6_nd_target(datapacket_t* pkt, uint128__t ipv6_nd_target)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_icmpv6_hdr(pack->headers,0))) return;
	set_icmpv6_neighbor_taddr(get_icmpv6_hdr(pack->headers,0), ipv6_nd_target);
}

void
platform_packet_set_ipv6_nd_sll(datapacket_t* pkt, uint64_t ipv6_nd_sll)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_icmpv6_hdr(pack->headers,0))) return;
	set_icmpv6_ll_saddr(get_icmpv6_hdr(pack->headers,0), ipv6_nd_sll);
}

void
platform_packet_set_ipv6_nd_tll(datapacket_t* pkt, uint64_t ipv6_nd_tll)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_icmpv6_hdr(pack->headers,0))) return;
	set_icmpv6_ll_taddr(get_icmpv6_hdr(pack->headers,0), ipv6_nd_tll);
}

void
platform_packet_set_ipv6_exthdr(datapacket_t* pkt, uint16_t ipv6_exthdr)
{
	/*TODO Extension headers not yet implemented*/
}

void
platform_packet_set_icmpv6_type(datapacket_t* pkt, uint8_t icmpv6_type)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_icmpv6_hdr(pack->headers,0))) return;
	set_icmpv6_type(get_icmpv6_hdr(pack->headers,0), icmpv6_type);
}

void
platform_packet_set_icmpv6_code(datapacket_t* pkt, uint8_t icmpv6_code)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_icmpv6_hdr(pack->headers,0))) return;
	set_icmpv6_code(get_icmpv6_hdr(pack->headers,0), icmpv6_code);
}

void
platform_packet_set_tcp_src(datapacket_t* pkt, uint16_t tcp_src)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_tcp_hdr(pack->headers,0))) return;
	set_tcp_sport(get_tcp_hdr(pack->headers,0), tcp_src);
	pack->tcp_recalc_checksum = true;
}

void
platform_packet_set_tcp_dst(datapacket_t* pkt, uint16_t tcp_dst)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_tcp_hdr(pack->headers,0))) return;
	set_tcp_dport(get_tcp_hdr(pack->headers,0), tcp_dst);
	pack->tcp_recalc_checksum = true;
}

void
platform_packet_set_udp_src(datapacket_t* pkt, uint16_t udp_src)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_udp_hdr(pack->headers,0))) return;
	set_udp_sport(get_udp_hdr(pack->headers,0), udp_src);
	pack->udp_recalc_checksum = true;
}

void
platform_packet_set_udp_dst(datapacket_t* pkt, uint16_t udp_dst)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_udp_hdr(pack->headers,0))) return;
	set_udp_dport(get_udp_hdr(pack->headers,0), udp_dst);
	pack->udp_recalc_checksum = true;
}

void
platform_packet_set_sctp_src(datapacket_t* pkt, uint16_t sctp_src)
{
	//TODO: implement
}

void
platform_packet_set_sctp_dst(datapacket_t* pkt, uint16_t sctp_dst)
{
	//TODO: implement
}

void
platform_packet_set_icmpv4_type(datapacket_t* pkt, uint8_t type)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_icmpv4_hdr(pack->headers,0))) return;
	set_icmpv4_type(get_icmpv4_hdr(pack->headers,0), type);
	pack->icmpv4_recalc_checksum = true;
}

void
platform_packet_set_icmpv4_code(datapacket_t* pkt, uint8_t code)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_icmpv4_hdr(pack->headers,0))) return;
	set_icmpv4_code(get_icmpv4_hdr(pack->headers,0), code);
	pack->icmpv4_recalc_checksum = true;
}


void
platform_packet_set_mpls_label(datapacket_t* pkt, uint32_t label)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_mpls_hdr(pack->headers,0))) return;
	set_mpls_label(get_mpls_hdr(pack->headers,0), label);
}

void
platform_packet_set_mpls_tc(datapacket_t* pkt, uint8_t tc)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_mpls_hdr(pack->headers,0))) return;
	set_mpls_tc(get_mpls_hdr(pack->headers,0), tc);
}
void
platform_packet_set_mpls_bos(datapacket_t* pkt, bool bos)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_mpls_hdr(pack->headers,0))) return;
	set_mpls_bos(get_mpls_hdr(pack->headers,0), bos);
}
void platform_packet_set_pbb_isid(datapacket_t*pkt, uint32_t pbb_isid)
{
	//TODO: implement
}
void platform_packet_set_tunnel_id(datapacket_t*pkt, uint64_t tunnel_id)
{
	//TODO: implement
}
void platform_packet_pop_pbb(datapacket_t* pkt, uint16_t ether_type)
{
	//TODO: implement
}
void platform_packet_push_pbb(datapacket_t* pkt, uint16_t ether_type)
{
	//TODO: implement
}
void
platform_packet_set_pppoe_type(datapacket_t* pkt, uint8_t type)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_pppoe_hdr(pack->headers,0))) return;
	set_pppoe_type(get_pppoe_hdr(pack->headers,0), type);
}

void
platform_packet_set_pppoe_code(datapacket_t* pkt, uint8_t code)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_pppoe_hdr(pack->headers,0))) return;
	set_pppoe_code(get_pppoe_hdr(pack->headers,0), code);
}

void
platform_packet_set_pppoe_sid(datapacket_t* pkt, uint16_t sid)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_pppoe_hdr(pack->headers,0))) return;
	set_pppoe_sessid(get_pppoe_hdr(pack->headers,0), sid);
}

void
platform_packet_set_ppp_proto(datapacket_t* pkt, uint16_t proto)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_ppp_hdr(pack->headers,0))) return;
	set_ppp_prot(get_ppp_hdr(pack->headers,0), proto);
}

void
platform_packet_set_gtp_msg_type(datapacket_t* pkt, uint8_t msg_type)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_gtpu_hdr(pack->headers,0))) return;
	set_gtpu_msg_type(get_gtpu_hdr(pack->headers,0), msg_type);
}

void
platform_packet_set_gtp_teid(datapacket_t* pkt, uint32_t teid)
{
	datapacket_dpdk_t *pack = (datapacket_dpdk_t*)pkt->platform_state;
	if ((NULL == pack) || (NULL == get_gtpu_hdr(pack->headers,0))) return;
	set_gtpu_teid(get_gtpu_hdr(pack->headers,0), teid);
}

void platform_packet_pop_gtp(datapacket_t* pkt)
{
	//TODO: implement
}
void platform_packet_push_gtp(datapacket_t* pkt)
{
	//TODO: implement
}


void
platform_packet_drop(datapacket_t* pkt)
{
	datapacket_dpdk_t* state = (datapacket_dpdk_t*)(pkt->platform_state);
	
	ROFL_DEBUG("Dropping packet(%p)\n",pkt);
	
	if ( NULL == state ){
		ROFL_DEBUG("packet state is NULL\n");
		return;
	}
	
	rte_pktmbuf_free(state->mbuf);
	
	if( state->packet_in_bufferpool ){
		//Release buffer only if the packet is stored there
		bufferpool::release_buffer(pkt);
	}
	
	return;
}

static void output_single_packet(datapacket_t* pkt, datapacket_dpdk_t* pack, switch_port_t* port){

	//Output packet to the appropiate queue and port_num
	if(likely(port && port->platform_port_state) && port->up && port->forward_packets){
		
		ROFL_DEBUG("[%s] OUTPUT packet(%p)\n", port->name, pkt);
#ifdef DEBUG
		dump_packet_matches(&pkt->matches);
#endif

		tx_pkt(port, pack->output_queue, pkt);	
	}else{
		rte_pktmbuf_free(((datapacket_dpdk_t*)pkt->platform_state)->mbuf);
	}

	if( ((datapacket_dpdk_t*)pkt->platform_state)->packet_in_bufferpool ){
		//Release buffer only if the packet is stored there
		bufferpool::release_buffer(pkt);
	}
}

/**
* Output packet to the port(s)
* The action HAS to implement the destruction/release of the pkt
* (including if the pkt is a replica).
*
* If a flooding output actions needs to be done, the function
* has itself to deal with packet replication.
*/
void platform_packet_output(datapacket_t* pkt, switch_port_t* output_port){

	of_switch_t const* sw;
	datapacket_dpdk_t* pack;

	if(!output_port){
		assert(0);
		return;
	}

	//Check whether dpx86 is NULL
	if (NULL == (pack = (datapacket_dpdk_t*)pkt->platform_state )){
		assert(0);
		return;
	}

	//IP Checksum recalculation
	if(pack->ipv4_recalc_checksum){
		if(get_ipv4_hdr(pack->headers,0))	
			ipv4_calc_checksum(get_ipv4_hdr(pack->headers,0));
	}

	//Outer most IPv4 frame
	void *fipv4 = get_ipv4_hdr(pack->headers,0);

	if ((pack->tcp_recalc_checksum) && get_tcp_hdr(pack->headers,0) && fipv4) {
		
		
		tcp_calc_checksum(
			get_tcp_hdr(pack->headers,0),
			get_ipv4_src(fipv4),
			get_ipv4_dst(fipv4),
			get_ipv4_proto(fipv4),
			get_pkt_len(pkt, pack->headers, get_tcp_hdr(pack->headers,0), NULL ) ); // start at innermost IPv4 up to and including last frame

	} else if ((pack->udp_recalc_checksum) && (get_udp_hdr(pack->headers,0)) && fipv4) {

		udp_calc_checksum(
			get_udp_hdr(pack->headers,0),
			get_ipv4_src(fipv4),
			get_ipv4_dst(fipv4),
			get_ipv4_proto(fipv4),
			get_pkt_len(pkt, pack->headers, get_udp_hdr(pack->headers,0), NULL ) ); // start at innermost IPv4 up to and including last frame

	} else if ((pack->icmpv4_recalc_checksum) && (get_icmpv4_hdr(pack->headers, 0))) {

		icmpv4_calc_checksum(
			get_icmpv4_hdr(pack->headers,0),
			get_pkt_len(pkt, pack->headers, get_icmpv4_hdr(pack->headers,0), NULL ) );
	}


	//flood_meta_port is a static variable defined in the physical_switch
	//the meta_port
	if(output_port == flood_meta_port || output_port == all_meta_port){ //We don't have STP, so it is the same
		datapacket_t* replica;
		switch_port_t* port_it;
		datapacket_dpdk_t* replica_pack;

		//Get switch
		sw = pkt->sw;	
		
		if(unlikely(!sw)){
			bufferpool::release_buffer(pkt);
			return;
		}
	
		//We need to flood
		for(unsigned i=0;i<LOGICAL_SWITCH_MAX_LOG_PORTS;++i){

			port_it = sw->logical_ports[i].port;

			//Check port is not incomming port, exists, and is up 
			if( (i == pack->in_port) || !port_it || port_it->no_flood)
				continue;

			//replicate packet
			replica = platform_packet_replicate(pkt); 	
			replica_pack = (datapacket_dpdk_t*)pkt->platform_state;

			ROFL_DEBUG("[%s] OUTPUT FLOOD packet(%p), origin(%p)\n", port_it->name, replica, pkt);
			
			//send the replica
			output_single_packet(replica, replica_pack, port_it);
		}

#ifdef DEBUG
		dump_packet_matches(&pkt->matches);
#endif
			
		//discard the original packet always (has been replicated)
		bufferpool::release_buffer(pkt);
	}else if(output_port == in_port_meta_port){
		
		//In port
		switch_port_t* port;
		sw = pkt->sw;	

		if(unlikely(pack->in_port >= LOGICAL_SWITCH_MAX_LOG_PORTS)){
			assert(0);
			return;
		}

		port = sw->logical_ports[pack->in_port].port;
		if( unlikely(port == NULL)){
			assert(0);
			return;
		
		}
	
		//Send to the incomming port 
		output_single_packet(pkt, pack, port);
	}else{
		//Single output	
		output_single_packet(pkt, pack, output_port);
	}

}

/**
* Creates a copy (in heap) of the datapacket_t structure.
* the platform specific state (->platform_state) is copied 
*  depending on the flag copy_mbuf
*/
datapacket_t* platform_packet_replicate__(datapacket_t* pkt, bool copy_mbuf){

	datapacket_t* pkt_replica=NULL;
	struct rte_mbuf* mbuf=NULL;
	datapacket_dpdk_t* pkt_dpdk;
	datapacket_dpdk_t* pkt_dpdk_replica;
	
	//Protect
	if(unlikely(!pkt))
		return NULL;

	//datapacket_t* pkt_replica;
	pkt_replica = bufferpool::get_free_buffer(false);
	
	if(unlikely(!pkt_replica)){
		ROFL_DEBUG("Replicate packet; could not clone pkt(%p). No buffers left in bufferpool\n", pkt);
		goto PKT_REPLICATE_ERROR;
	}

	pkt_dpdk = (datapacket_dpdk_t*)pkt->platform_state;
	pkt_dpdk_replica = (datapacket_dpdk_t*)pkt_replica->platform_state;

	if( copy_mbuf==true ){
		//Retrieve an mbuf, copy contents, and initialize pkt_dpdk_replica
		mbuf = rte_pktmbuf_clone(((datapacket_dpdk_t*)pkt->platform_state)->mbuf, pool_direct); 
		
		if(unlikely(!mbuf)){
			ROFL_DEBUG("Replicate packet; could not clone pkt(%p). rte_pktmbuf_clone failed\n", pkt);
			ROFL_DEBUG("errno %d - %s\n", rte_errno, rte_strerror(rte_errno));
			goto PKT_REPLICATE_ERROR;
		}
	} else {
		//only moving the pointer
		mbuf = ((datapacket_dpdk_t*)pkt->platform_state)->mbuf;
		//NOTE here we could do ((datapacket_dpdk_t*)pkt->platform_state)->mbuf = NULL;
		
	}

	//Copy PKT stuff
	memcpy(&pkt_replica->matches, &pkt->matches, sizeof(pkt->matches));
	memcpy(&pkt_replica->write_actions, &pkt->write_actions ,sizeof(pkt->write_actions));

	//mark as replica
	pkt_replica->is_replica = true;
	pkt_replica->sw = pkt->sw;

	//Initialize replica buffer and classify  //TODO: classification state could be copied
	init_datapacket_dpdk(pkt_dpdk_replica, mbuf, (of_switch_t*)pkt->sw, pkt_dpdk->in_port, 0, true, true);

	//Replicate the packet(copy contents)	
	pkt_dpdk_replica->ipv4_recalc_checksum 	= pkt_dpdk->ipv4_recalc_checksum;
	pkt_dpdk_replica->icmpv4_recalc_checksum 	= pkt_dpdk->icmpv4_recalc_checksum;
	pkt_dpdk_replica->tcp_recalc_checksum 	= pkt_dpdk->tcp_recalc_checksum;
	pkt_dpdk_replica->udp_recalc_checksum 	= pkt_dpdk->udp_recalc_checksum;

	return pkt_replica; //DO NOT REMOVE

PKT_REPLICATE_ERROR:
	assert(0); 
	
	//Release packet
	if(pkt_replica){
		bufferpool::release_buffer(pkt_replica);

		if(mbuf){
			rte_pktmbuf_free(mbuf);
		}
	}

	return NULL;
}

/**
* Creates a copy (in heap) of the datapacket_t structure including any
* platform specific state (->platform_state). The following behaviour
* is expected from this hook:
* 
* - All data fields and pointers of datapacket_t struct must be memseted to 0, except:
* - datapacket_t flag is_replica must be set to true
* - platform_state, if used, must be replicated (copied) otherwise NULL
*
*/
datapacket_t* platform_packet_replicate(datapacket_t* pkt){
	return platform_packet_replicate__(pkt, true);
}

