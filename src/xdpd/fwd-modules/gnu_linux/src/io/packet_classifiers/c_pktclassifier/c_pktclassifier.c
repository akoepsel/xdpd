#include "c_pktclassifier.h"
//#include "../packetclassifier.h"

#include "./headers/cpc_arpv4.h"
#include "./headers/cpc_ethernet.h"
#include "./headers/cpc_gtpu.h"
#include "./headers/cpc_icmpv4.h"
#include "./headers/cpc_icmpv6.h"
#include "./headers/cpc_ipv4.h"
#include "./headers/cpc_ipv6.h"
#include "./headers/cpc_mpls.h"
#include "./headers/cpc_ppp.h"
#include "./headers/cpc_pppoe.h"
#include "./headers/cpc_tcp.h"
#include "./headers/cpc_udp.h"
#include "./headers/cpc_vlan.h"

// Constants
//Maximum header occurrences per type
const unsigned int MAX_ETHER_FRAMES = 2;
const unsigned int MAX_VLAN_FRAMES = 4;
const unsigned int MAX_MPLS_FRAMES = 16;
const unsigned int MAX_ARPV4_FRAMES = 1;
const unsigned int MAX_IPV4_FRAMES = 2;
const unsigned int MAX_ICMPV4_FRAMES = 2;
const unsigned int MAX_IPV6_FRAMES = 2;
const unsigned int MAX_ICMPV6_FRAMES = 2;
const unsigned int MAX_UDP_FRAMES = 2;
const unsigned int MAX_TCP_FRAMES = 2;
const unsigned int MAX_SCTP_FRAMES = 2;
const unsigned int MAX_PPPOE_FRAMES = 1;
const unsigned int MAX_PPP_FRAMES = 1;
const unsigned int MAX_GTP_FRAMES = 1;

//Total maximum header occurrences
const unsigned int MAX_HEADERS = MAX_ETHER_FRAMES +
						MAX_VLAN_FRAMES +
						MAX_MPLS_FRAMES +
						MAX_ARPV4_FRAMES +
						MAX_IPV4_FRAMES +
						MAX_ICMPV4_FRAMES +
						MAX_IPV6_FRAMES +
						MAX_ICMPV6_FRAMES +
						MAX_UDP_FRAMES +
						MAX_TCP_FRAMES +
						MAX_SCTP_FRAMES +
						MAX_PPPOE_FRAMES + 
						MAX_PPP_FRAMES +
						MAX_GTP_FRAMES;


//Relative positions within the array;
const unsigned int FIRST_ETHER_FRAME_POS = 0; //Very first frame always
const unsigned int FIRST_VLAN_FRAME_POS = FIRST_ETHER_FRAME_POS+MAX_ETHER_FRAMES;
const unsigned int FIRST_MPLS_FRAME_POS = FIRST_VLAN_FRAME_POS+MAX_VLAN_FRAMES;
const unsigned int FIRST_ARPV4_FRAME_POS = FIRST_MPLS_FRAME_POS+MAX_MPLS_FRAMES;
const unsigned int FIRST_IPV4_FRAME_POS = FIRST_ARPV4_FRAME_POS+MAX_ARPV4_FRAMES;
const unsigned int FIRST_ICMPV4_FRAME_POS = FIRST_IPV4_FRAME_POS+MAX_IPV4_FRAMES;
const unsigned int FIRST_IPV6_FRAME_POS = FIRST_ICMPV4_FRAME_POS+MAX_ICMPV4_FRAMES;
const unsigned int FIRST_ICMPV6_FRAME_POS = FIRST_IPV6_FRAME_POS+MAX_IPV6_FRAMES;
const unsigned int FIRST_UDP_FRAME_POS = FIRST_ICMPV6_FRAME_POS+MAX_ICMPV6_FRAMES;
const unsigned int FIRST_TCP_FRAME_POS = FIRST_UDP_FRAME_POS+MAX_UDP_FRAMES;
const unsigned int FIRST_SCTP_FRAME_POS = FIRST_TCP_FRAME_POS+MAX_TCP_FRAMES;
const unsigned int FIRST_PPPOE_FRAME_POS = FIRST_SCTP_FRAME_POS+MAX_SCTP_FRAMES;
const unsigned int FIRST_PPP_FRAME_POS = FIRST_PPPOE_FRAME_POS+MAX_PPPOE_FRAMES;
const unsigned int FIRST_GTP_FRAME_POS = FIRST_PPP_FRAME_POS+MAX_PPP_FRAMES;

//Just to be on the safe side of life
//assert( (FIRST_PPP_FRAME_POS + MAX_PPP_FRAMES) == MAX_HEADERS);

typedef struct classify_state{
	//Real container
	header_container_t headers[MAX_HEADERS];
	
	//Counters
	unsigned int num_of_headers[HEADER_TYPE_MAX];
	
	//Flag to know if it is classified
	bool is_classified;

	//Inner most (last) ethertype
	uint16_t eth_type;

}classify_state_t;


//NOTE create and destroy?

classify_state_t* create_classifier(){
	return malloc();
};

void classify_reset(){
	//memset 0
}

void destroy_classifier(){
	//free
}

/// Getters
cpc_eth_hdr_t* ether(classify_state_t* clas_state, int idx){
	unsigned int pos;
	if(idx > (int)MAX_ETHER_FRAMES)
		return NULL;

	if(idx < 0) //Inner most
		pos = FIRST_ETHER_FRAME_POS + clas_state->num_of_headers[HEADER_TYPE_ETHER] - 1;
	else
		pos = FIRST_ETHER_FRAME_POS + idx;	

	//Return the index
	if(clas_state->headers[pos].present)
		return (cpc_eth_hdr_t*) clas_state->headers[pos].frame;	
	return NULL;
}

cpc_vlan_hdr_t* vlan(classify_state_t* clas_state, int idx){
	unsigned int pos;	

	if(idx > (int)MAX_VLAN_FRAMES)
		return NULL;

	if(idx < 0) //Inner most
		pos = FIRST_VLAN_FRAME_POS + clas_state->num_of_headers[HEADER_TYPE_VLAN] - 1;
	else
		pos = FIRST_VLAN_FRAME_POS + idx;	

	//Return the index
	if(clas_state->headers[pos].present)
		return (cpc_vlan_hdr_t*) clas_state->headers[pos].frame;
	return NULL;
}

cpc_mpls_hdr_t* mpls(classify_state_t* clas_state, int idx){
	unsigned int pos;	

	if(idx > (int)MAX_MPLS_FRAMES)
		return NULL;

	if(idx < 0) //Inner most
		pos = FIRST_MPLS_FRAME_POS + clas_state->num_of_headers[HEADER_TYPE_MPLS] - 1;
	else
		pos = FIRST_MPLS_FRAME_POS + idx;	

	//Return the index
	if(clas_state->headers[pos].present)
		return (cpc_mpls_hdr_t*) clas_state->headers[pos].frame;
	return NULL;
}

cpc_arpv4_hdr_t* arpv4(classify_state_t* clas_state, int idx){
	unsigned int pos;	

	if(idx > (int)MAX_ARPV4_FRAMES)
		return NULL;

	if(idx < 0) //Inner most
		pos = FIRST_ARPV4_FRAME_POS + clas_state->num_of_headers[HEADER_TYPE_ARPV4] - 1;
	else
		pos = FIRST_ARPV4_FRAME_POS + idx;	

	//Return the index
	if(clas_state->headers[pos].present)
		return (cpc_arpv4_hdr_t*) clas_state->headers[pos].frame;
	return NULL;
}

cpc_ipv4_hdr_t* ipv4(classify_state_t* clas_state, int idx){
	unsigned int pos;	

	if(idx > (int)MAX_IPV4_FRAMES)
		return NULL;

	if(idx < 0) //Inner most const
		pos = FIRST_IPV4_FRAME_POS + clas_state->num_of_headers[HEADER_TYPE_IPV4] - 1;
	else
		pos = FIRST_IPV4_FRAME_POS + idx;	

	//Return the index
	if(clas_state.headers[pos].present)
		return (cpc_ipv4_hdr_t*) clas_state->headers[pos].frame;
	return NULL;
}

cpc_icmpv4_hdr_t* icmpv4(classify_state_t* clas_state, int idx){
	unsigned int pos;	

	if(idx > (int)MAX_ICMPV4_FRAMES)
		return NULL;

	if(idx < 0) //Inner most
		pos = FIRST_ICMPV4_FRAME_POS + clas_state->num_of_headers[HEADER_TYPE_ICMPV4] - 1;
	else
		pos = FIRST_ICMPV4_FRAME_POS + idx;	

	//Return the index
	if(clas_state->headers[pos].present)
		return (cpc_icmpv4_hdr_t*) clas_state->headers[pos].frame;
	return NULL;

}


cpc_ipv6_hdr_t* ipv6(classify_state_t* clas_state, int idx){
	unsigned int pos;	

	if(idx > (int)MAX_IPV6_FRAMES)
		return NULL;

	if(idx < 0) //Inner most
		pos = FIRST_IPV6_FRAME_POS + clas_state->num_of_headers[HEADER_TYPE_IPV6] - 1;
	else
		pos = FIRST_IPV6_FRAME_POS + idx;

	//Return the index
	if(clas_state->headers[pos].present)
		return (cpc_ipv6_hdr_t*) clas_state->headers[pos].frame;
	return NULL;
}

cpc_icmpv6_hdr_t* icmpv6(classify_state_t* clas_state, int idx){
	unsigned int pos;	

	if(idx > (int)MAX_ICMPV6_FRAMES)
		return NULL;

	if(idx < 0) //Inner most
		pos = FIRST_ICMPV6_FRAME_POS + clas_state->num_of_headers[HEADER_TYPE_ICMPV6] - 1;
	else
		pos = FIRST_ICMPV6_FRAME_POS + idx;	

	//Return the index
	if(clas_state->headers[pos].present)
		return (cpc_icmpv6_hdr_t*) clas_state->headers[pos].frame;
	return NULL;

}

cpc_udp_hdr_t* udp(classify_state_t* clas_state, int idx){
	unsigned int pos;	

	if(idx > (int)MAX_UDP_FRAMES)
		return NULL;

	if(idx < 0) //Inner most
		pos = FIRST_UDP_FRAME_POS + clas_state->num_of_headers[HEADER_TYPE_UDP] - 1;
	else
		pos = FIRST_UDP_FRAME_POS + idx;	

	//Return the index
	if(clas_state->headers[pos].present)
		return (cpc_udp_hdr_t*) clas_state->headers[pos].frame;
	return NULL;
}

cpc_tcp_hdr_t* tcp(classify_state_t* clas_state, int idx){
	unsigned int pos;	

	if(idx > (int)MAX_TCP_FRAMES)
		return NULL;

	if(idx < 0) //Inner most
		pos = FIRST_TCP_FRAME_POS + clas_state->num_of_headers[HEADER_TYPE_TCP] - 1;
	else
		pos = FIRST_TCP_FRAME_POS + idx;	

	//Return the index
	if(clas_state->headers[pos].present)
		return (cpc_tcp_hdr_t*) clas_state->headers[pos].frame;
	return NULL;
}

cpc_pppoe_hdr_t* pppoe(classify_state_t* clas_state, int idx){
	unsigned int pos;	

	if(idx > (int)MAX_PPPOE_FRAMES)
		return NULL;

	if(idx < 0) //Inner most
		pos = FIRST_PPPOE_FRAME_POS + clas_state->num_of_headers[HEADER_TYPE_PPPOE] - 1;
	else
		pos = FIRST_PPPOE_FRAME_POS + idx;	

	//Return the index
	if(clas_state->headers[pos].present)
		return (cpc_pppoe_hdr_t*) clas_state->headers[pos].frame;
	return NULL;
}

cpc_ppp_hdr_t* ppp(classify_state_t* clas_state, int idx){
	unsigned int pos;	

	if(idx > (int)MAX_PPP_FRAMES)
		return NULL;

	if(idx < 0) //Inner most
		pos = FIRST_PPP_FRAME_POS + clas_state->num_of_headers[HEADER_TYPE_PPP] - 1;
	else
		pos = FIRST_PPP_FRAME_POS + idx;	

	//Return the index
	if(clas_state->headers[pos].present)
		return (cpc_ppp_hdr_t*) clas_state->headers[pos].frame;
	return NULL;
}

cpc_gtpu_base_hdr_t* gtp(classify_state_t* clas_state, int idx){
	unsigned int pos;

	if(idx > (int)MAX_GTP_FRAMES)
		return NULL;

	if(idx < 0) //Inner most
		pos = FIRST_GTP_FRAME_POS + clas_state->num_of_headers[HEADER_TYPE_GTP] - 1;
	else
		pos = FIRST_GTP_FRAME_POS + idx;

	//Return the index
	if(clas_state->headers[pos].present)
		return (cpc_gtpu_base_hdr_t*) clas_state->headers[pos].frame;
	return NULL;
}





/// Classify part





void classify(datapacketx86 *pkt){
	if(is_classified)
		classify_reset();
	parse_ethernet(pkt->get_buffer(),pkt->get_buffer_length());
	is_classified = true;
}

void parse_ethernet(classify_state_t* clas_state, uint8_t *data, size_t datalen){
	if (datalen < sizeof(struct cpc_eth_hdr_t)){return;}

	//Set frame
	unsigned int num_of_ether = clas_state->num_of_headers[HEADER_TYPE_ETHER];
	//clas_state->headers[FIRST_ETHER_FRAME_POS + num_of_ether].frame->reset(data, datalen/*sizeof(struct rofl::fetherframe::eth_hdr_t)*/);
	clas_state->headers[FIRST_ETHER_FRAME_POS + num_of_ether].frame = (cpc_eth_hdr_t *)data;
	clas_state->headers[FIRST_ETHER_FRAME_POS + num_of_ether].present = true;
	clas_state->num_of_headers[HEADER_TYPE_ETHER] = num_of_ether+1;

	//Increment pointers and decrement remaining payload size
	data += sizeof(struct cpc_eth_hdr_t);
	datalen -= sizeof(struct cpc_eth_hdr_t);

	//Set pointer to header
	cpc_eth_hdr_t* ether_header = (cpc_eth_hdr_t*) clas_state->headers[FIRST_ETHER_FRAME_POS + num_of_ether].frame;
	
	clas_state->eth_type = get_dl_type(ether_header);//WARNING ether and vlan have the same function defined!!

	switch (clas_state->eth_type) {
		case VLAN_CTAG_ETHER:
		case VLAN_STAG_ETHER:
		case VLAN_ITAG_ETHER:
			{
				parse_vlan(clas_state, data, datalen);
			}
			break;
		case MPLS_ETHER:
		case MPLS_ETHER_UPSTREAM:
			{
				parse_mpls(clas_state, data, datalen);
			}
			break;
		case PPPOE_ETHER_DISCOVERY:
		case PPPOE_ETHER_SESSION:
			{
				parse_pppoe(clas_state, data, datalen);
			}
			break;
		case ARPV4_ETHER:
			{
				parse_arpv4(clas_state, data, datalen);
			}
			break;
		case IPV4_ETHER:
			{
				parse_ipv4(clas_state, data, datalen);
			}
			break;
		case IPV6_ETHER:
			{
				parse_ipv6(clas_state, data,datalen);
			}
			break;
		default:
			{
				
			}
			break;
	}
}

void parse_vlan(classify_state_t* clas_state, uint8_t *data, size_t datalen){
	if (datalen < sizeof(struct cpc_vlan_hdr_t)) { return; }

	//Set frame
	unsigned int num_of_vlan = clas_state->num_of_headers[HEADER_TYPE_VLAN];
	//clas_state->headers[FIRST_VLAN_FRAME_POS + num_of_vlan].frame->reset(data, datalen);
	clas_state->headers[FIRST_VLAN_FRAME_POS + num_of_vlan].frame = (cpc_vlan_hdr_t*)data;
	clas_state->headers[FIRST_VLAN_FRAME_POS + num_of_vlan].present = true;
	clas_state->num_of_headers[HEADER_TYPE_VLAN] = num_of_vlan+1;

	//Increment pointers and decrement remaining payload size
	data += sizeof(struct cpc_vlan_hdr_t);
	datalen -= sizeof(struct cpc_vlan_hdr_t);

	//Set pointer to header
	cpc_vlan_hdr_t* vlan = (cpc_vlan_hdr_t*) clas_state->headers[FIRST_VLAN_FRAME_POS + num_of_vlan].frame;
	
	clas_state->eth_type = get_dl_type(vlan); //WARNING this is the VLAN one

	switch (clas_state->eth_type) {
		case VLAN_CTAG_ETHER:
		case VLAN_STAG_ETHER:
		case VLAN_ITAG_ETHER:
			{
				parse_vlan(clas_state, data, datalen);
			}
			break;
		case MPLS_ETHER:
		case MPLS_ETHER_UPSTREAM:
			{
				parse_mpls(clas_state, data, datalen);
			}
			break;
		case PPPOE_ETHER_DISCOVERY:
		case PPPOE_ETHER_SESSION:
			{
				parse_pppoe(clas_state, data, datalen);
			}
			break;
		case ARPV4_ETHER:
			{
				parse_arpv4(clas_state, data, datalen);
			}
			break;
		case IPV4_ETHER:
			{
				parse_ipv4(clas_state, data, datalen);
			}
			break;
		default:
			{

			}
			break;
	}
}

void parse_mpls(classify_state_t* clas_state, uint8_t *data, size_t datalen){
	if (datalen < sizeof(struct cpc_mpls_hdr_t)) { return; }

	//Set frame
	unsigned int num_of_mpls = clas_state->num_of_headers[HEADER_TYPE_MPLS];
	//clas_state->headers[FIRST_MPLS_FRAME_POS + num_of_mpls].frame->reset(data, datalen);
	clas_state->headers[FIRST_MPLS_FRAME_POS + num_of_mpls].frame = (cpc_mpls_hdr_t*) data;
	clas_state->headers[FIRST_MPLS_FRAME_POS + num_of_mpls].present = true;
	clas_state->num_of_headers[HEADER_TYPE_MPLS] = num_of_mpls+1;

	//Increment pointers and decrement remaining payload size
	data += sizeof(struct cpc_mpls_hdr_t);
	datalen -= sizeof(struct cpc_mpls_hdr_t);

	//Set pointer to header
	cpc_mpls_hdr_t* mpls = (cpc_mpls_hdr_t*) clas_state->headers[FIRST_MPLS_FRAME_POS + num_of_mpls].frame;
	
	if (! get_mpls_bos(mpls)){

		parse_mpls(clas_state,data, datalen);

	}else{
		
		//TODO: We could be trying to guess if payload is IPv4/v6 and continue parsing...
	}
}
void parse_pppoe(classify_state_t* clas_state, uint8_t *data, size_t datalen){
	if (datalen < sizeof(struct rofl::fpppoeframe::pppoe_hdr_t)) { return; }

	//Set frame
	unsigned int num_of_pppoe = clas_state->num_of_headers[HEADER_TYPE_PPPOE];
	//headers[FIRST_PPPOE_FRAME_POS + num_of_pppoe].frame->reset(data, datalen);
	clas_state->headers[FIRST_PPPOE_FRAME_POS + num_of_pppoe].frame = (cpc_pppoe_hdr_t*)data;
	clas_state->headers[FIRST_PPPOE_FRAME_POS + num_of_pppoe].present = true;
	clas_state->num_of_headers[HEADER_TYPE_PPPOE] = num_of_pppoe+1;
	
	cpc_mpls_hdr_t* pppoe = (cpc_mpls_hdr_t*) clas_state->headers[FIRST_PPPOE_FRAME_POS + num_of_pppoe].frame;

	switch (clas_state->eth_type) {
		case PPPOE_ETHER_DISCOVERY:
			{
				datalen -= sizeof(struct cpc_pppoe_hdr_t);

				uint16_t pppoe_len = get_pppoe_length() > datalen ? datalen : get_pppoe_length();

				/*
				 * parse any pppoe service tags
				 */
				pppoe->unpack(data, sizeof(struct cpc_pppoe_hdr_t) + pppoe_len);


				/*
				 * any remaining bytes after the pppoe tags => padding?
				 */
				if (datalen > pppoe->tags.length())
				{
					//TODO: Continue parsing??	
				}
			}
			break;
		case PPPOE_ETHER_SESSION:
			{
				//Increment pointers and decrement remaining payload size
				data += sizeof(struct cpc_pppoe_hdr_t);
				datalen -= sizeof(struct cpc_pppoe_hdr_t);

				parse_ppp(clas_state,data, datalen);
			}
			break;
		default:
			{
				// log error?
			}
			break;
	}

}

void parse_ppp(classify_state_t* clas_state, uint8_t *data, size_t datalen){
	if (datalen < sizeof(struct cpc_ppp_hdr_t)) { return; }

	//Set frame
	unsigned int num_of_ppp = clas_state->num_of_headers[HEADER_TYPE_PPP];
	//clas_state->headers[FIRST_PPP_FRAME_POS + num_of_ppp].frame->reset(data, datalen);
	clas_state->headers[FIRST_PPP_FRAME_POS + num_of_ppp].frame = (cpc_ppp_hdr_t*) data;
	clas_state->headers[FIRST_PPP_FRAME_POS + num_of_ppp].present = true;
	clas_state->num_of_headers[HEADER_TYPE_PPP] = num_of_ppp+1;

	//Set reference
	cpc_ppp_hdr_t* ppp = (cpc_ppp_hdr_t*) clas_state->headers[FIRST_PPP_FRAME_POS + num_of_ppp].frame; 

	//Increment pointers and decrement remaining payload size
	switch (get_ppp_prot(ppp)) {
		case PPP_PROT_IPV4:
			{
				//Increment pointers and decrement remaining payload size
				data += sizeof(struct cpc_ppp_hdr_t);
				datalen -= sizeof(struct cpc_ppp_hdr_t);

				parse_ipv4(data, datalen);
			}
			break;
		default:
			{
				ppp->unpack(data, datalen);
			}
			break;
	}
}

void parse_arpv4(classify_state_t* clas_state, uint8_t *data, size_t datalen){
	if (datalen < sizeof(struct cpc_arpv4_hdr_t)) { return; }

	//Set frame
	unsigned int num_of_arpv4 = clas_state->num_of_headers[HEADER_TYPE_ARPV4];
	//clas_state->headers[FIRST_ARPV4_FRAME_POS + num_of_arpv4].frame->reset(data, datalen);
	clas_state->headers[FIRST_ARPV4_FRAME_POS + num_of_arpv4].frame = (cpc_arpv4_hdr_t*) data;
	clas_state->headers[FIRST_ARPV4_FRAME_POS + num_of_arpv4].present = true;
	clas_state->num_of_headers[HEADER_TYPE_ARPV4] = num_of_arpv4+1;

	//Set reference
	//rofl::farpv4frame *arpv4 = headers[FIRST_ARPV4_FRAME_POS + num_of_arpv4].frame; 

	//Increment pointers and decrement remaining payload size
	data += sizeof(struct cpc_arpv4_hdr_t);
	datalen -= sizeof(struct cpc_arpv4_hdr_t);

	if (datalen > 0){
		//TODO: something?
	}
}

void parse_ipv4(classify_state_t* clas_state, uint8_t *data, size_t datalen){
	if (datalen < sizeof(struct rofl::fipv4frame::ipv4_hdr_t)) { return; }
	
	//Set frame
	unsigned int num_of_ipv4 = clas_state->num_of_headers[HEADER_TYPE_IPV4];
	//clas_state->headers[FIRST_IPV4_FRAME_POS + num_of_ipv4].frame->reset(data, datalen);
	clas_state->headers[FIRST_IPV4_FRAME_POS + num_of_ipv4].frame = (cpc_ipv4_hdr_t*) data;
	clas_state->headers[FIRST_IPV4_FRAME_POS + num_of_ipv4].present = true;
	clas_state->num_of_headers[HEADER_TYPE_IPV4] = num_of_ipv4+1;

	//Set reference
	cpc_ipv4_hdr_t *ipv4 = (cpc_ipv4_hdr_t*) clas_state->headers[FIRST_IPV4_FRAME_POS + num_of_ipv4].frame; 

	//Increment pointers and decrement remaining payload size
	data += sizeof(struct cpc_ipv4_hdr_t);
	datalen -= sizeof(struct cpc_ipv4_hdr_t);

	if (has_MF_bit_set(ipv4)){
		// TODO: fragment handling

		return;
	}

	// FIXME: IP header with options


	switch (get_ipv4_proto(ipv4)) {
		case IPV4_IP_PROTO:
			{
				parse_ipv4(clas_state, data, datalen);
			}
			break;
		case ICMPV4_IP_PROTO:
			{
				parse_icmpv4(clas_state, data, datalen);
			}
			break;
		case UDP_IP_PROTO:
			{
				parse_udp(clas_state, data, datalen);
			}
			break;
		case TCP_IP_PROTO:
			{
				parse_tcp(clas_state, data, datalen);
			}
			break;
#if 0
		case SCTP_IP_PROTO:
			{
				parse_sctp(clas_state, data, datalen);
			}
			break;
#endif
		default:
			{
			
			}
			break;
	}
}

void parse_icmpv4(classify_state_t* clas_state, uint8_t *data, size_t datalen){
	if (datalen < sizeof(struct cpc_icmpv4_hdr_t)) { return; }

	//Set frame
	unsigned int num_of_icmpv4 = clas_state->num_of_headers[HEADER_TYPE_ICMPV4];
	//headers[FIRST_ICMPV4_FRAME_POS + num_of_icmpv4].frame->reset(data, datalen);
	clas_state->headers[FIRST_ICMPV4_FRAME_POS + num_of_icmpv4].frame = (cpc_icmpv4_hdr_t*) data;
	clas_state->headers[FIRST_ICMPV4_FRAME_POS + num_of_icmpv4].present = true;
	clas_state->num_of_headers[HEADER_TYPE_ICMPV4] = num_of_icmpv4+1;

	//Set reference
	//rofl::ficmpv4frame *icmpv4 = (rofl::ficmpv4frame*) headers[FIRST_ICMPV4_FRAME_POS + num_of_icmpv4].frame; 

	//Increment pointers and decrement remaining payload size
	data += sizeof(struct cpc_icmpv4_hdr_t);
	datalen -= sizeof(struct cpc_icmpv4_hdr_t);


	if (datalen > 0){
		//TODO: something?	
	}
}

void parse_ipv6(classify_state_t* clas_state, uint8_t *data, size_t datalen){
	if (datalen < sizeof(struct cpc_ipv6_hdr_t)) { return; }
	
	//Set frame
	unsigned int num_of_ipv6 = clas_state->num_of_headers[HEADER_TYPE_IPV6];
	//clas_state->headers[FIRST_IPV6_FRAME_POS + num_of_ipv6].frame->reset(data, datalen);
	clas_state->headers[FIRST_IPV6_FRAME_POS + num_of_ipv6].frame = (cpc_ipv6_hdr_t*) data;
	clas_state->headers[FIRST_IPV6_FRAME_POS + num_of_ipv6].present = true;
	clas_state->num_of_headers[HEADER_TYPE_IPV6] = num_of_ipv6+1;

	//Set reference
	cpc_ipv6_hdr_t *ipv6 = (cpc_ipv6_hdr_t*) clas_state->headers[FIRST_IPV6_FRAME_POS + num_of_ipv6].frame; 

	//Increment pointers and decrement remaining payload size
	data += sizeof(struct cpc_ipv6_hdr_t);
	datalen -= sizeof(struct cpc_ipv6_hdr_t);

	// FIXME: IP header with options

	switch (get_next_header(ipv6)) {
		case IPV4_IP_PROTO:
			{
				parse_ipv4(data, datalen);
			}
			break;
		case ICMPV4_IP_PROTO:
			{
				parse_icmpv4(data, datalen);
			}
			break;
		case IPV6_IP_PROTO:
			{
				parse_ipv6(data, datalen);
			}
			break;
		case ICMPV6_IP_PROTO:
			{
				parse_icmpv6(data, datalen);
			}
			break;
		case UDP_IP_PROTO:
			{
				parse_udp(data, datalen);
			}
			break;
		case TCP_IP_PROTO:
			{
				parse_tcp(data, datalen);
			}
			break;
#if 0
		case SCTP_IP_PROTO:
			{
				parse_sctp(data, datalen);
			}
			break;
#endif
		default:
			{
			
			}
			break;
	}
}

void parse_icmpv6(classify_state_t* clas_state, uint8_t *data, size_t datalen){
	if (datalen < sizeof(struct cpc_icmpv6_hdr_t)) { return; }

	//Set frame
	unsigned int num_of_icmpv6 = clas_state->num_of_headers[HEADER_TYPE_ICMPV6];
	//clas_state->headers[FIRST_ICMPV6_FRAME_POS + num_of_icmpv6].frame->reset(data, datalen);
	clas_state->headers[FIRST_ICMPV6_FRAME_POS + num_of_icmpv6].frame = (cpc_icmpv6_hdr_t*)data;
	clas_state->headers[FIRST_ICMPV6_FRAME_POS + num_of_icmpv6].present = true;
	clas_state->num_of_headers[HEADER_TYPE_ICMPV6] = num_of_icmpv6+1;

	//Set reference
	//rofl::ficmpv6frame *icmpv6 = (rofl::ficmpv6frame*) headers[FIRST_ICMPV6_FRAME_POS + num_of_icmpv6].frame; 

	//Increment pointers and decrement remaining payload size
	data += sizeof(struct cpc_icmpv6_hdr_t);
	datalen -= sizeof(struct cpc_icmpv6_hdr_t);


	if (datalen > 0){
		//TODO: something?	
	}
}

void parse_tcp(classify_state_t* clas_state, uint8_t *data, size_t datalen){
	if (datalen < sizeof(struct cpc_tcp_hdr_t)) { return; }

	//Set frame
	unsigned int num_of_tcp = clas_state->num_of_headers[HEADER_TYPE_TCP];
	//clas_state->headers[FIRST_TCP_FRAME_POS + num_of_tcp].frame->reset(data, datalen);
	clas_state->headers[FIRST_TCP_FRAME_POS + num_of_tcp].frame = (cpc_tcp_hdr_t*) data;
	clas_state->headers[FIRST_TCP_FRAME_POS + num_of_tcp].present = true;
	clas_state->num_of_headers[HEADER_TYPE_TCP] = num_of_tcp+1;

	//Set reference
	//rofl::ftcpframe *tcp = (rofl::ftcpframe*) headers[FIRST_TCP_FRAME_POS + num_of_tcp].frame; 

	//Increment pointers and decrement remaining payload size
	data += sizeof(struct cpc_tcp_hdr_t);
	datalen -= sizeof(struct cpc_tcp_hdr_t);

	if (datalen > 0){
		//TODO: something 
	}
}

void parse_udp(classify_state_t* clas_state, uint8_t *data, size_t datalen){
	if (datalen < sizeof(struct cpc_udp_hdr_t)) { return; }

	//Set frame
	unsigned int num_of_udp = clas_state->num_of_headers[HEADER_TYPE_UDP];
	//clas_state->headers[FIRST_UDP_FRAME_POS + num_of_udp].frame->reset(data, datalen);
	clas_state->headers[FIRST_UDP_FRAME_POS + num_of_udp].frame = (cpc_udp_hdr_t*) data;
	clas_state->headers[FIRST_UDP_FRAME_POS + num_of_udp].present = true;
	clas_state->num_of_headers[HEADER_TYPE_UDP] = num_of_udp+1;

	//Set reference
	cpc_udp_hdr_t *udp = (cpc_udp_hdr_t*) clas_state->headers[FIRST_UDP_FRAME_POS + num_of_udp].frame;
	
	//Increment pointers and decrement remaining payload size
	data += sizeof(struct cpc_udp_hdr_t);
	datalen -= sizeof(struct cpc_udp_hdr_t);

	if (datalen > 0){
		switch (get_dport(udp)) {
		case GTPU_UDP_PORT: {
			parse_gtp(data, datalen);
		} break;
		default: {
			//TODO: something
		} break;
		}
	}
}

void parse_gtp(classify_state_t* clas_state, uint8_t *data, size_t datalen){
	if (datalen < sizeof(struct cpc_gtpu_base_hdr_t)) { return; }

	//Set frame
	unsigned int num_of_gtp = clas_state->num_of_headers[HEADER_TYPE_GTP];
	//clas_state->headers[FIRST_GTP_FRAME_POS + num_of_gtp].frame->reset(data, datalen);
	clas_state->headers[FIRST_GTP_FRAME_POS + num_of_gtp].frame = (cpc_gtpu_base_hdr_t*) data;
	clas_state->headers[FIRST_GTP_FRAME_POS + num_of_gtp].present = true;
	clas_state->num_of_headers[HEADER_TYPE_GTP] = num_of_gtp+1;

	//Set reference
	//rofl::fgtpuframe *gtp = (rofl::fgtpuframe*) headers[FIRST_GTP_FRAME_POS + num_of_gtp].frame;

	//Increment pointers and decrement remaining payload size
	data += sizeof(struct cpc_gtpu_base_hdr_t);
	datalen -= sizeof(struct cpc_gtpu_base_hdr_t);

	if (datalen > 0){
		//TODO: something
	}
}


//..


void pop_header(){}
void push_header(){}
//pop...
//push...

void dump(){}

size_t get_pkt_len(rofl::fframe *from, rofl::fframe *to){}

