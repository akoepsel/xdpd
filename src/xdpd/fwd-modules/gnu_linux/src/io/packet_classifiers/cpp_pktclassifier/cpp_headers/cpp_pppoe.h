#ifndef _CPP_PPPOE_H_
#define _CPP_PPPOE_H_

#include <stdint.h>

uint8_t get_pppoe_vers(void *hdr);

void set_pppoe_vers(void *hdr, uint8_t version);

uint8_t get_pppoe_type(void *hdr);

void set_pppoe_type(void *hdr, uint8_t type);

uint8_t get_pppoe_code(void *hdr);

void set_pppoe_code(void *hdr, uint8_t code);

uint16_t get_pppoe_sessid(void *hdr);

void set_pppoe_sessid(void *hdr, uint16_t sessid);

uint16_t get_pppoe_length(void *hdr);

void set_pppoe_length(void *hdr, uint16_t length);

#endif //_CPP_PPPOE_H_
