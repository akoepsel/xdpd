#ifndef _CPP_VLAN_H_
#define _CPP_VLAN_H_

#include <stdint.h>

void set_dl_vlan_id(void* hdr, uint16_t vid);

uint16_t get_dl_vlan_id(void* hdr);

void set_dl_vlan_pcp(void* hdr, uint8_t pcp);

uint16_t get_dl_vlan_pcp(void* hdr);

void set_dl_vlan_cfi(void* hdr, bool cfi);

bool get_dl_vlan_cfi(void* hdr);

void set_dl_vlan_type(void* hdr, uint16_t dl_type);

uint16_t get_dl_vlan_type(void* hdr);


#endif //_CPP_VLAN_H_
