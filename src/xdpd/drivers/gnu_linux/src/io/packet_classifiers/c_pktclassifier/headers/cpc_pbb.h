/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _CPC_PBB_H_
#define _CPC_PBB_H_

#include <rofl/datapath/pipeline/common/protocol_constants.h>

/**
* @file cpc_pbb.h
* @author Andreas Koepsel<andreas.koepsel (at) bisdn.de>
*
* @brief Structure definitions and inline getters and setters for Provider Backbone Bridges (IEEE 802.1ah)
*/

#define DEFAULT_ETHER_FRAME_SIZE 1518
#define PBB_ETH_ALEN 6

/* Ethernet constants and definitions */

// Provider Backbone Bridges (IEEE 802.1ah) header
// I-TAG => chapter 9.7
struct cpc_pbb_hdr {
	uint32_t 	i_tag;
	uint8_t 	c_da[PBB_ETH_ALEN];
	uint8_t 	c_sa[PBB_ETH_ALEN];
	uint16_t 	c_dltype;
	uint8_t 	data[0];
}__attribute__((packed));

typedef struct cpc_pbb_hdr cpc_pbb_hdr_t;

inline static
uint32_t* get_pbb_isid(void* hdr){ // returns i_tab in network byte order, tci in byte 0, isid in bytes 1,2,3
	return (uint32_t*) &(((cpc_pbb_hdr_t*)hdr)->i_tag) ;
};

inline static
void set_pbb_isid(void* hdr, uint32_t i_tag){ // i_tag in network byte order, isid in bytes 1,2,3, tci field in byte 0
	uint32_t *ptr = (uint32_t*) &((cpc_pbb_hdr_t*)hdr)->i_tag;
	*ptr = ((*ptr) & ~OF1X_3_BYTE_MASK) | (i_tag & OF1X_3_BYTE_MASK);
};


inline static
uint64_t* get_pbb_c_da(void *hdr){
	return (uint64_t*) &((cpc_pbb_hdr_t*)hdr)->c_da;
};

inline static
void set_pbb_c_da(void* hdr, uint64_t c_da){
	uint64_t *ptr = (uint64_t *) &((cpc_pbb_hdr_t*)hdr)->c_da;
	*ptr = (p_da & OF1X_6_BYTE_MASK) | (*ptr & ~OF1X_6_BYTE_MASK);
};

inline static
uint64_t* get_pbb_c_sa(void* hdr){
	return (uint64_t*) &((cpc_pbb_hdr_t*)hdr)->c_sa;
};

inline static
void set_pbb_c_sa(void* hdr, uint64_t c_sa){
	uint64_t *ptr = (uint64_t *) &((cpc_pbb_hdr_t*)hdr)->c_sa;
	*ptr = (c_sa & OF1X_6_BYTE_MASK) | (*ptr & ~OF1X_6_BYTE_MASK);
};

inline static
uint16_t* get_pbb_c_dltype(void* hdr){
	return &((cpc_pbb_hdr_t *)hdr)->c_dltype;
};

inline static
void set_pbb_c_dltype(void* hdr, uint16_t dltype){
	((cpc_pbb_hdr_t *)hdr)->c_dltype = dltype;
};

#endif //_CPC_PBB_H_
