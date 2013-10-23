#ifndef _CPC_ICMPV6_H_
#define _CPC_ICMPV6_H_

#include <rofl/common/endian_conversion.h>
#include <rofl/datapath/pipeline/common/large_types.h>
#include "cpc_ethernet.h"
#include "../../../../util/likely.h"
#include "../../../../util/compiler_assert.h"


#define IPV6_ADDR_LEN		16
#define ETHER_ADDR_LEN		6

struct stat;
enum icmpv6_option_type_t {
	ICMPV6_OPT_LLADDR_SOURCE 		= 1,
	ICMPV6_OPT_LLADDR_TARGET 		= 2,
	ICMPV6_OPT_PREFIX_INFO			= 3,
	ICMPV6_OPT_REDIRECT				= 4,
	ICMPV6_OPT_MTU					= 5,
};

/* ICMPv6 generic option header */
struct cpc_icmpv6_option_hdr_t {
	uint8_t 						type;
	uint8_t							len;
	uint8_t 						data[0];
} __attribute__((packed));


/* ICMPv6 link layer address option */
struct cpc_icmpv6_lla_option_t {
	struct icmpv6_option_hdr_t		hdr;
	uint8_t							addr[ETHER_ADDR_LEN]; // len=1 (in 8-octets wide blocks) and we assume Ethernet here
} __attribute__((packed));

/* ICMPv6 prefix information option */
struct cpc_icmpv6_prefix_info_t {
	struct icmpv6_option_hdr_t		hdr;
	uint8_t							pfxlen;
	uint8_t							flags;
	uint32_t						valid_lifetime;
	uint32_t						preferred_lifetime;
	uint32_t						reserved;
	uint8_t							prefix[IPV6_ADDR_LEN];
} __attribute__((packed));

/* ICMPv6 redirected option header */
struct cpc_icmpv6_redirected_hdr_t {
	struct icmpv6_option_hdr_t		hdr;
	uint8_t							reserved[6];
	uint8_t							data[0];
} __attribute__((packed));

/* ICMPv6 MTU option */
struct cpc_icmpv6_mtu_t {
	struct icmpv6_option_hdr_t		hdr;
	uint8_t							reserved[2];
	uint32_t						mtu;
} __attribute__((packed));

typedef union icmpv6optu{
	struct cpc_icmpv6_option_hdr_t		*optu;
	struct cpc_icmpv6_lla_option_t		*optu_lla;
	struct cpc_icmpv6_prefix_info_t	*optu_pfx;
	struct cpc_icmpv6_redirected_hdr_t	*optu_rdr;
	struct cpc_icmpv6_mtu_t				*optu_mtu;
} cpc_icmpv6optu_t;

enum icmpv6_ip_proto_t {
	ICMPV6_IP_PROTO = 58,
};

enum icmpv6_type_t {
	ICMPV6_TYPE_DESTINATION_UNREACHABLE 							= 1,
	ICMPV6_TYPE_PACKET_TOO_BIG										= 2,
	ICMPV6_TYPE_TIME_EXCEEDED										= 3,
	ICMPV6_TYPE_PARAMETER_PROBLEM									= 4,
	ICMPV6_TYPE_ECHO_REQUEST										= 128,
	ICMPV6_TYPE_ECHO_REPLY											= 129,
	ICMPV6_TYPE_MULTICAST_LISTENER_QUERY							= 130,
	ICMPV6_TYPE_MULTICAST_LISTENER_REPORT							= 131,
	ICMPV6_TYPE_MULTICAST_LISTENER_DONE							= 132,
	ICMPV6_TYPE_ROUTER_SOLICATION									= 133,
	ICMPV6_TYPE_ROUTER_ADVERTISEMENT								= 134,
	ICMPV6_TYPE_NEIGHBOR_SOLICITATION								= 135,
	ICMPV6_TYPE_NEIGHBOR_ADVERTISEMENT								= 136,
	ICMPV6_TYPE_REDIRECT_MESSAGE									= 137,
	ICMPV6_TYPE_ROUTER_RENUMBERING									= 138,
	ICMPV6_TYPE_ICMP_NODE_INFORMATION_QUERY						= 139,
	ICMPV6_TYPE_ICMP_NODE_INFORMATION_RESPONSE						= 140,
	ICMPV6_TYPE_INVERSE_NEIGHBOR_DISCOVERY_SOLICITATION_MESSAGE 	= 141,
	ICMPV6_TYPE_INVERSE_NEIGHBOR_DISCOVERY_ADVERTISEMENT_MESSAGE 	= 142,
	ICMPV6_TYPE_MULTICAST_LISTENER_DISCOVERY_REPORT				= 143,
	ICMPV6_TYPE_HOME_AGENT_ADDRESS_DISCOVERY_REQUEST_MESSAGE		= 144,
	ICMPV6_TYPE_HOME_AGENT_ADDRESS_DISCOVERY_REPLY_MESSAGE			= 145,
	ICMPV6_TYPE_MOBILE_PREFIX_SOLICITATION							= 146,
	ICMPV6_TYPE_MOBILE_PREFIX_ADVERTISEMENT						= 147,
};

enum icmpv6_destination_unreachable_code_t {
	ICMPV6_DEST_UNREACH_CODE_NO_ROUTE_TO_DESTINATION										= 0,
	ICMPV6_DEST_UNREACH_CODE_COMMUNICATION_WITH_DESTINATION_ADMINISTRATIVELY_PROHIBITED	= 1,
	ICMPV6_DEST_UNREACH_CODE_BEYOND_SCOPE_OF_SOURCE_ADDRESS								= 2,
	ICMPV6_DEST_UNREACH_CODE_ADDRESS_UNREACHABLE											= 3,
	ICMPV6_DEST_UNREACH_CODE_PORT_UNREACHABLE												= 4,
	ICMPV6_DEST_UNREACH_CODE_SOURCE_ADDRESS_FAILED_INGRESS_EGRESS_POLICY					= 5,
	ICMPV6_DEST_UNREACH_CODE_REJECT_ROUTE_TO_DESTINATION									= 6,
	ICMPV6_DEST_UNREACH_CODE_ERROR_IN_SOURCE_ROUTING_HEADER								= 7,
};


/**
	* ICMPv6 message types
	*/

/* ICMPv6 generic header */
struct cpc_icmpv6_hdr_t {
	uint8_t 						type;
	uint8_t 						code;
	uint16_t 						checksum;
	uint8_t 						data[0];
} __attribute__((packed));


/**
	* ICMPv6 error message types
	*/

/* ICMPv6 message format for Destination Unreachable */
struct cpc_icmpv6_dest_unreach_hdr_t {
	struct cpc_icmpv6_hdr_t			icmpv6_hdr;				// type=133, code=0
	uint32_t						unused;					// a 32bit value
	uint8_t							data[0];				// the IP packet
} __attribute__((packed));

/* ICMPv6 message format for Packet Too Big */
struct cpc_icmpv6_pkt_too_big_hdr_t {
	struct cpc_icmpv6_hdr_t			icmpv6_hdr;				// type=133, code=0
	uint32_t						unused;					// a 32bit value
	uint8_t							data[0];				// the IP packet
} __attribute__((packed));

/* ICMPv6 message format for Time Exceeded */
struct cpc_icmpv6_time_exceeded_hdr_t {
	struct cpc_icmpv6_hdr_t			icmpv6_hdr;				// type=133, code=0
	uint32_t						unused;					// a 32bit value
	uint8_t							data[0];				// the IP packet
} __attribute__((packed));

/* ICMPv6 message format for Parameter Problem */
struct cpc_icmpv6_param_problem_hdr_t {
	struct cpc_icmpv6_hdr_t			icmpv6_hdr;				// type=133, code=0
	uint32_t						pointer;				// a 32bit value
	uint8_t							data[0];				// the IP packet
} __attribute__((packed));

/* ICMPv6 echo request message format */
struct cpc_icmpv6_echo_request_hdr_t {
	struct cpc_icmpv6_hdr_t			icmpv6_hdr;				// type=133, code=0
	uint16_t						id;
	uint16_t 						seqno;
	uint8_t							data[0];				// arbitrary data
} __attribute__((packed));

/* ICMPv6 echo reply message format */
struct cpc_icmpv6_echo_reply_hdr_t {
	struct cpc_icmpv6_hdr_t			icmpv6_hdr;				// type=133, code=0
	uint16_t						id;
	uint16_t 						seqno;
	uint8_t							data[0];				// arbitrary data
} __attribute__((packed));

/**
	* ICMPv6 NDP message types
	*/

/* ICMPv6 router solicitation */
struct cpc_icmpv6_router_solicitation_hdr_t {
	struct cpc_icmpv6_hdr_t			icmpv6_hdr;				// type=133, code=0
	uint32_t 						reserved;				// reserved for later use, for now: mbz
	struct cpc_icmpv6_option_hdr_t	options[0];
} __attribute__((packed));

/* ICMPv6 router advertisement */
struct cpc_icmpv6_router_advertisement_hdr_t {
	struct cpc_icmpv6_hdr_t			icmpv6_hdr;				// type=134, code=0
	uint8_t 						cur_hop_limit;
	uint8_t							flags;
	uint16_t 						rtr_lifetime;
	uint32_t						reachable_timer;
	uint32_t 						retrans_timer;
	struct cpc_icmpv6_option_hdr_t	options[0];
} __attribute__((packed));

/* ICMPv6 neighbor solicitation */
struct cpc_icmpv6_neighbor_solicitation_hdr_t {
	struct cpc_icmpv6_hdr_t			icmpv6_hdr;				// type=135, code=0
	uint32_t 						reserved;				// reserved for later use, for now: mbz
	uint8_t							taddr[IPV6_ADDR_LEN]; 	// =target address
	struct cpc_icmpv6_option_hdr_t	options[0];
} __attribute__((packed));

/* ICMPv6 neighbor advertisement */
struct cpc_icmpv6_neighbor_advertisement_hdr_t {
	struct cpc_icmpv6_hdr_t			icmpv6_hdr;				// type=136, code=0
	uint32_t 						flags;
	uint8_t							taddr[IPV6_ADDR_LEN]; 	// =target address
	struct cpc_icmpv6_option_hdr_t	options[0];
} __attribute__((packed));

/* ICMPv6 redirect message */
struct cpc_icmpv6_redirect_hdr_t {
	struct cpc_icmpv6_hdr_t			icmpv6_hdr;				// type=137, code=0
	uint32_t 						reserved;				// reserved for later use, for now: mbz
	uint8_t							taddr[IPV6_ADDR_LEN]; 	// =target address
	uint8_t							daddr[IPV6_ADDR_LEN];	// =destination address
	struct cpc_icmpv6_option_hdr_t	options[0];
} __attribute__((packed));


/**
	* ICMPv6 pseudo header
	*/
/* for ICMPv6 checksum calculation */
struct cpc_icmpv6_pseudo_hdr_t {
	uint8_t 	src[IPV6_ADDR_LEN];
	uint8_t 	dst[IPV6_ADDR_LEN];
	uint32_t 	icmpv6_len;				// payload length (extension headers + ICMPv6 message)
	uint8_t 	zeros[3];				// = 0
	uint8_t 	nxthdr;					// = 58 (=ICMPV6_IP_PROTO, see below)
} __attribute__((packed));

#define DEFAULT_ICMPV6_FRAME_SIZE sizeof(struct icmpv6_hdr_t)

typedef union cpc_icmpv6u{
	struct cpc_icmpv6_hdr_t 						*icmpv6u_hdr;							// ICMPv6 message header
	struct cpc_icmpv6_dest_unreach_hdr_t			*icmpv6u_dst_unreach_hdr;				// ICMPv6 destination unreachable
	struct cpc_icmpv6_pkt_too_big_hdr_t			*icmpv6u_pkt_too_big_hdr;				// ICMPv6 packet too big
	struct cpc_icmpv6_time_exceeded_hdr_t			*icmpv6u_time_exceeded_hdr;				// ICMPv6 time exceeded
	struct cpc_icmpv6_param_problem_hdr_t			*icmpv6u_param_problem_hdr;				// ICMPv6 parameter problem
	struct cpc_icmpv6_echo_request_hdr_t			*icmpv6u_echo_request_hdr;				// ICMPv6 echo request
	struct cpc_icmpv6_echo_reply_hdr_t				*icmpv6u_echo_reply_hdr;				// ICMPv6 echo reply
	struct cpc_icmpv6_router_solicitation_hdr_t	*icmpv6u_rtr_solicitation_hdr;			// ICMPv6 rtr solicitation
	struct cpc_icmpv6_router_advertisement_hdr_t	*icmpv6u_rtr_advertisement_hdr;		// ICMPv6 rtr advertisement
	struct cpc_icmpv6_neighbor_solicitation_hdr_t	*icmpv6u_neighbor_solication_hdr;		// ICMPv6 NDP solication header
	struct cpc_icmpv6_neighbor_advertisement_hdr_t	*icmpv6u_neighbor_advertisement_hdr;	// ICMPv6 NDP advertisement header
	struct cpc_icmpv6_redirect_hdr_t				*icmpv6u_redirect_hdr;					// ICMPV6 redirect header
} cpc_icmpv6u_t;



inline static
void icmpv6_calc_checksum(void *hdr, uint16_t length){
	//TODO Implement the checksum 
};

inline static
uint8_t icmpv6_get_option(void *hdr, uint8_t type){
	//TODO ... return the option of the specified type
	return 0;
};

//NOTE initialize, parse ..?¿

inline static
uint8_t get_icmpv6_code(void *hdr){
	return ((cpc_icmpv6u_t*)hdr)->icmpv6u_hdr->code;
};

inline static
void set_icmpv6_code(void *hdr, uint8_t code){
	((cpc_icmpv6u_t*)hdr)->icmpv6u_hdr->code = code;
};

inline static
uint8_t get_icmpv6_type(void *hdr){
	return ((cpc_icmpv6u_t*)hdr)->icmpv6u_hdr->type;
};

inline static
void set_icmpv6_type(void *hdr, uint8_t type){
	((cpc_icmpv6u_t*)hdr)->icmpv6u_hdr->type = type;
};

inline static
uint128__t get_icmpv6_neighbor_taddr(void *hdr){
	uint128__t addr;
	switch (get_icmpv6_type(hdr)) {
		case ICMPV6_TYPE_NEIGHBOR_SOLICITATION:
			addr= (uint128__t)((cpc_icmpv6u_t*)hdr)->icmpv6u_neighbor_solication_hdr->taddr;
			break;
		case ICMPV6_TYPE_NEIGHBOR_ADVERTISEMENT:
			addr= (uint128__t)((cpc_icmpv6u_t*)hdr)->icmpv6u_neighbor_advertisement_hdr->taddr;
			break;
		case ICMPV6_TYPE_REDIRECT_MESSAGE:
			addr= (uint128__t)((cpc_icmpv6u_t*)hdr)->icmpv6u_redirect_hdr->taddr;
			break;
		default:
			//TODO LOG ERROR
			assert(0);
			break;
	}
#if __BYTE_ORDER == __LITTLE_ENDIAN
	SWAP_U128(addr); //be128toh
#endif
	return addr;
};

inline static
void set_icmpv6_neighbor_taddr(void *hdr, uint128__t taddr){
	uint128__t *ptr;
	switch (get_icmpv6_type(hdr)) {
		case ICMPV6_TYPE_NEIGHBOR_SOLICITATION:
			ptr= (uint128__t*)&((cpc_icmpv6u_t*)hdr)->icmpv6u_neighbor_solication_hdr->taddr;
			break;
		case ICMPV6_TYPE_NEIGHBOR_ADVERTISEMENT:
			ptr= (uint128__t*)&((cpc_icmpv6u_t*)hdr)->icmpv6u_neighbor_advertisement_hdr->taddr;
			break;
		case ICMPV6_TYPE_REDIRECT_MESSAGE:
			ptr= (uint128__t*)&((cpc_icmpv6u_t*)hdr)->icmpv6u_redirect_hdr->taddr;
			break;
		default:
			//TODO LOG ERROR
			assert(0);
			break;
	}
#if __BYTE_ORDER == __LITTLE_ENDIAN
	SWAP_U128(taddr); //htobe128
#endif
	*ptr = taddr;
};

//ndp_rtr_flag
//ndp_solicited_flag
//ndp_override_flag
//neighbor_taddr



inline static
uint8_t get_icmpv6_opt_type(void *hdr){
	return ((cpc_icmpv6optu_t*)hdr)->optu->type;
};

inline static
void set_icmpv6_opt_type(void *hdr, uint8_t type){
	((cpc_icmpv6optu_t*)hdr)->optu->type = type;
};

inline static
uint64_t get_ll_taddr(void *hdr){
	if(unlikely(ICMPV6_OPT_LLADDR_TARGET != ((cpc_icmpv6optu_t*)hdr)->optu->type)){
		assert(0);
		return 0;
	}
	
	return mac_addr_to_u64(((cpc_icmpv6optu_t*)hdr)->optu_lla->addr);
};

inline static
void set_ll_taddr(void *hdr, uint64_t taddr){
	if(unlikely(ICMPV6_OPT_LLADDR_TARGET != ((icmpv6optu*)hdr)->optu->type)){
		assert(0);
	}
	u64_to_mac_ptr(((cpc_icmpv6optu_t*)hdr)->optu_lla->addr,taddr);
};

inline static
uint64_t get_ll_saddr(void *hdr){
	if(unlikely(ICMPV6_OPT_LLADDR_SOURCE != ((cpc_icmpv6optu_t*)hdr)->optu->type)){
		assert(0);
		return 0;
	}
	
	return mac_addr_to_u64(((cpc_icmpv6optu_t*)hdr)->optu_lla->addr);
};

inline static
void set_ll_saddr(void *hdr, uint64_t saddr){
	if(unlikely(ICMPV6_OPT_LLADDR_SOURCE != ((cpc_icmpv6optu_t*)hdr)->optu->type)){
		assert(0);
	}
	u64_to_mac_ptr(((cpc_icmpv6optu_t*)hdr)->optu_lla->addr,saddr);
};

inline static
uint8_t get_pfx_on_link_flag(void *hdr){
	if(unlikely(ICMPV6_OPT_PREFIX_INFO != ((cpc_icmpv6optu_t*)hdr)->optu->type)){
		assert(0);
		return 0;
	}
	return ( (((cpc_icmpv6optu_t*)hdr)->optu_pfx->flags & 0x80) >> 7 );
};

inline static
void set_pfx_on_link_flag(void *hdr, uint8_t flag){
	if(unlikely(ICMPV6_OPT_PREFIX_INFO != ((cpc_icmpv6optu_t*)hdr)->optu->type)){
		assert(0);
		return 0;
	}
	((cpc_icmpv6optu_t*)hdr)->optu_pfx->flags = (((cpc_icmpv6optu_t*)hdr)->optu_pfx->flags & 0x7F) | ((flag & 0x01) << 7);
};

inline static
uint8_t get_pfx_aac_flag(void *hdr){
	if(unlikely(ICMPV6_OPT_PREFIX_INFO != ((cpc_icmpv6optu_t*)hdr)->optu->type)){
		assert(0);
		return 0;
	}
	return ((((cpc_icmpv6optu_t*)hdr)->optu_pfx->flags & 0x40) >> 6);
};

inline static
void 









#endif //_CPC_ICMPV6_H_