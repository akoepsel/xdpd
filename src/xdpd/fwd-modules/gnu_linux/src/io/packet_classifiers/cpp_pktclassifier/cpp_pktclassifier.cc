#include "cpp_pktclassifier.h"

#include "rofl_pktclassifier.h"
#include "static_pktclassifier.h"

//typedef struct classify_state pktclassifier;
typedef static_pktclassifier pktclassifier;

struct classify_state* init_classifier(){
	//call constructor
	return (struct classify_state*) new static_pktclassifier();
}

void destroy_classifier(struct classify_state* clas_state){
	//Call destructor
}

void classify_packet(struct classify_state* clas_state, uint8_t* pkt, size_t len){
	((pktclassifier*)clas_state)->classify(pkt, len);
}

void reset_classifier(struct classify_state* clas_state){
	((pktclassifier*)clas_state)->classify_reset();
}



void* ether(struct classify_state* clas_state, int idx){
	return ((pktclassifier*)clas_state)->ether(idx);
}

void* vlan(struct classify_state* clas_state, int idx){
	return ((pktclassifier*)clas_state)->vlan(idx);
}

void* mpls(struct classify_state* clas_state, int idx){
	return ((pktclassifier*)clas_state)->mpls(idx);
}

void* arpv4(struct classify_state* clas_state, int idx){
	return ((pktclassifier*)clas_state)->arpv4(idx);
}

void* ipv4(struct classify_state* clas_state, int idx){
	return ((pktclassifier*)clas_state)->ipv4(idx);
}

void* icmpv4(struct classify_state* clas_state, int idx){
	return ((pktclassifier*)clas_state)->icmpv4(idx);
}

void* ipv6(struct classify_state* clas_state, int idx){
	return ((pktclassifier*)clas_state)->ipv6(idx);
}

void* icmpv6(struct classify_state* clas_state, int idx){
	return ((pktclassifier*)clas_state)->icmpv6(idx);
}

void* udp(struct classify_state* clas_state, int idx){
	return ((pktclassifier*)clas_state)->udp(idx);
}

void* tcp(struct classify_state* clas_state, int idx){
	return ((pktclassifier*)clas_state)->tcp(idx);
}

void* pppoe(struct classify_state* clas_state, int idx){
	return ((pktclassifier*)clas_state)->pppoe(idx);
}

void* ppp(struct classify_state* clas_state, int idx){
	return ((pktclassifier*)clas_state)->ppp(idx);
}

void* gtp(struct classify_state* clas_state, int idx){
	return ((pktclassifier*)clas_state)->gtp(idx);
}


//push & pop
void pop_vlan(datapacket_t* pkt, struct classify_state* clas_state){
	((pktclassifier*)clas_state)->pop_vlan(pkt);
}

void pop_mpls(datapacket_t* pkt, struct classify_state* clas_state, uint16_t ether_type){
	((pktclassifier*)clas_state)->pop_mpls(pkt, ether_type);
}

void pop_pppoe(datapacket_t* pkt, struct classify_state* clas_state, uint16_t ether_type){
	((pktclassifier*)clas_state)->pop_pppoe(pkt, ether_type);
}

void pop_gtp(datapacket_t* pkt, struct classify_state* clas_state, uint16_t ether_type){
	//TODO ((pktclassifier*)clas_state)->pop_gtp();
}


void* push_vlan(datapacket_t* pkt, struct classify_state* clas_state, uint16_t ether_type){
	return ((pktclassifier*)clas_state)->push_vlan(pkt, ether_type);
}

void* push_mpls(datapacket_t* pkt, struct classify_state* clas_state, uint16_t ether_type){
	return ((pktclassifier*)clas_state)->push_mpls(pkt, ether_type);
}

void* push_pppoe(datapacket_t* pkt, struct classify_state* clas_state, uint16_t ether_type){
	return ((pktclassifier*)clas_state)->push_pppoe(pkt, ether_type);
}

void* push_gtp(datapacket_t* pkt, struct classify_state* clas_state, uint16_t ether_type){
	//TODO ((pktclassifier*)clas_state)->push_gtp();
	return NULL;
}

size_t get_pkt_len(datapacket_t* pkt, struct classify_state* clas_state, void *from, void *to){
	return ((pktclassifier*)clas_state)->get_pkt_len(pkt, from, to);
}
