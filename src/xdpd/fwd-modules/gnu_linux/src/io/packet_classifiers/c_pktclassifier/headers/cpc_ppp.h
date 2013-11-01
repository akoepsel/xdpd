#ifndef _CPC_PPP_H_
#define _CPC_PPP_H_

#include "../cpc_utils.h"

// HDLC related definitions
//
enum hdlc_code_t {
	HDLC_FRAME_DELIMITER = 0x7e,
	HDLC_DST_ALL = 0xff,
	HDLC_PPP_CONTROL = 0x03, // 0x03 => 0x7d 0x23, check RFC 1662
	HDLC_ESCAPE = 0x7d,
};

// PPP frame related definitions
//

enum ppp_prot_t {
	PPP_PROT_PADDING 	= 0x0001, // 0x00 0x01 in network byte order
	PPP_PROT_LCP 		= 0xc021, // 0xc0 0x21 in network byte order
	PPP_PROT_PAP 		= 0xc023, // 0xc0 0x23 in network byte order
	PPP_PROT_LQR 		= 0xc025, // 0xc0 0x25 in network byte order
	PPP_PROT_CHAP 		= 0xc223, // 0xc2 0x23 in network byte order
	PPP_PROT_EAP 		= 0xc227, // 0xc2 0x27 in network byte order
	PPP_PROT_IPCP 		= 0x8021, // 0x80 0x21 in network byte order
	PPP_PROT_IPV4 		= 0x0021, // 0x00 0x21 in network byte order
	PPP_PROT_IPV6CP 	= 0x8057, // 0x80 0x57 in network byte order
	PPP_PROT_IPV6 		= 0x0057, // 0x00 0x57 in network byte order
	PPP_PROT_CCP 		= 0x80fd, // 0x80 0xfd in network byte order
};

struct cpc_ppp_hdr_t {
	uint16_t prot;
	uint8_t data[0];
} __attribute__((packed));

// PPP-LCP related definitions
//
enum ppp_lcp_code_t {
	PPP_LCP_CONF_REQ = 0x01,
	PPP_LCP_CONF_ACK = 0x02,
	PPP_LCP_CONF_NAK = 0x03,
	PPP_LCP_CONF_REJ = 0x04,
	PPP_LCP_TERM_REQ = 0x05,
	PPP_LCP_TERM_ACK = 0x06,
	PPP_LCP_CODE_REJ = 0x07,
	PPP_LCP_PROT_REJ = 0x08,
	PPP_LCP_ECHO_REQ = 0x09,
	PPP_LCP_ECHO_REP = 0x0a,
	PPP_LCP_DISC_REQ = 0x0b,
};

/**
	* PPP-IPCP related definitions
	*
	* Code	Description			References
	* 0	Vendor Specific.	RFC 2153
	* 1	Configure-Request.
	* 2	Configure-Ack.
	* 3	Configure-Nak.
	* 4	Configure-Reject.
	* 5	Terminate-Request.
	* 6	Terminate-Ack.
	* 7	Code-Reject.
	*
	* (http://en.wikipedia.org/wiki/Internet_Protocol_Control_Protocol)
	*/
enum ppp_ipcp_code_t {
	PPP_IPCP_VEND_SPC = 0x00,//!< PPP_IPCP_VEND_SPC
	PPP_IPCP_CONF_REQ = 0x01,//!< PPP_IPCP_CONF_REQ
	PPP_IPCP_CONF_ACK = 0x02,//!< PPP_IPCP_CONF_ACK
	PPP_IPCP_CONF_NAK = 0x03,//!< PPP_IPCP_CONF_NAK
	PPP_IPCP_CONF_REJ = 0x04,//!< PPP_IPCP_CONF_REJ
	PPP_IPCP_TERM_REQ = 0x05,//!< PPP_IPCP_TERM_REQ
	PPP_IPCP_TERM_ACK = 0x06,//!< PPP_IPCP_TERM_ACK
	PPP_IPCP_CODE_REJ = 0x07 //!< PPP_IPCP_CODE_REJ
};

/* structure for lcp and ipcp */
struct cpc_ppp_lcp_hdr_t {
	uint8_t code;
	uint8_t ident;
	uint16_t length; // includes this header and data
	uint8_t data[0];
} __attribute__((packed));

enum ppp_lcp_option_t {
	PPP_LCP_OPT_RESERVED 	= 0x00,
	PPP_LCP_OPT_MRU 		= 0x01,
	PPP_LCP_OPT_ACCM 		= 0x02,
	PPP_LCP_OPT_AUTH_PROT 	= 0x03,
	PPP_LCP_OPT_QUAL_PROT 	= 0x04,
	PPP_LCP_OPT_MAGIC_NUM 	= 0x05,
	PPP_LCP_OPT_PFC 		= 0x07,
	PPP_LCP_OPT_ACFC 		= 0x08,
};

/**
	* Option	Length	Description						References
	* 1	 			IP-Addresses (deprecated).		RFC 1332
	* 2		>= 14	IP-Compression-Protocol.		RFC 1332, RFC 3241,
	* 													RFC 3544
	* 3		6		IP-Address.						RFC 1332
	* 4		6		Mobile-IPv4.					RFC 2290
	* 129		6		Primary DNS Server Address.		RFC 1877
	* 130		6		Primary NBNS Server Address.	RFC 1877
	* 131		6		Secondary DNS Server Address.	RFC 1877
	* 132		6		Secondary NBNS Server Address.	RFC 1877
	*
	* (http://en.wikipedia.org/wiki/Internet_Protocol_Control_Protocol#Configuration_Options)
	*/
enum ppp_ipcp_option_t {
	PPP_IPCP_OPT_IPV4_DEP	= 1,   //!< PPP_IPCP_OPT_IPV4_DEP
	PPP_IPCP_OPT_IP_COMP	= 2,   //!< PPP_IPCP_OPT_IP_COMP
	PPP_IPCP_OPT_IPV4		= 3,   //!< PPP_IPCP_OPT_IPV4		RFC 1332
	PPP_IPCP_OPT_MOB_IPV4	= 4,   //!< PPP_IPCP_OPT_MOB_IPV4 	RFC 2290
	PPP_IPCP_OPT_PRIM_DNS	= 129, //!< PPP_IPCP_OPT_PRIM_DNS
	PPP_IPCP_OPT_PRIM_MBNS	= 130, //!< PPP_IPCP_OPT_PRIM_MBNS
	PPP_IPCP_OPT_SEC_DNS	= 131, //!< PPP_IPCP_OPT_SEC_DNS
	PPP_IPCP_OPT_SEC_MBNS	= 132  //!< PPP_IPCP_OPT_SEC_MBNS
};

/* structure for lcp */
struct cpc_ppp_lcp_opt_hdr_t {
	uint8_t option;
	uint8_t length; // includes this header and data
	uint8_t data[0];
} __attribute__((packed));

/* structure for ipcp */
struct cpc_ppp_ipcp_opt_hdr_t {
	uint8_t option;
	uint8_t length; // includes this header and data
	uint8_t data[0];
} __attribute__((packed));

inline static
uint16_t get_ppp_prot(void *hdr) const
{
	return CPC_BE16TOH(((cpc_ppp_hdr*)hdr)->prot);
}

inline static
void set_ppp_prot(void *hdr, uint16_t prot)
{
	((cpc_ppp_hdr*)hdr)->prot = CPC_HTOBE16(prot);
}

inline static
uint8_t get_lcp_code(void *hdr)
{
	if (0 == ((cpc_ppp_lcp_hdr*)hdr)) throw ePPPLcpNotFound();

	return ((cpc_ppp_lcp_hdr*)hdr)->code;
}

inline static
void set_lcp_code(void *hdr, uint8_t code)
{
	if (0 == ((cpc_ppp_lcp_hdr*)hdr)) throw ePPPLcpNotFound();

	((cpc_ppp_lcp_hdr*)hdr)->code = code;
}

inline static
uint8_t get_lcp_ident(void *hdr)
{
	if (0 == ((cpc_ppp_lcp_hdr*)hdr)) throw ePPPLcpNotFound();

	return ((cpc_ppp_lcp_hdr*)hdr)->ident;
}

inline static
void set_lcp_ident(void *hdr, uint8_t ident)
{
	if (0 == ((cpc_ppp_lcp_hdr*)hdr)) throw ePPPLcpNotFound();

	((cpc_ppp_lcp_hdr*)hdr)->ident = ident;
}

inline static
uint16_t get_lcp_length(void *hdr)
{
	if (0 == ((cpc_ppp_lcp_hdr*)hdr)) throw ePPPLcpNotFound();

	return CPC_BE16TOH(((cpc_ppp_lcp_hdr*)hdr)->length);
}

inline static
void set_lcp_length(void *hdr, uint16_t len)
{
	if (0 == ((cpc_ppp_lcp_hdr*)hdr)) throw ePPPLcpNotFound();

	((cpc_ppp_lcp_hdr*)hdr)->length = CPC_HTOBE16(len);
}

inline static
fppp_lcp_option* get_lcp_option(void *hdr, enum ppp_lcp_option_t option)
{
	if (lcp_options.find(option) == lcp_options.end()) throw ePPPLcpOptionNotFound();

	return lcp_options[option];
}

inline static
uint8_t get_ipcp_code(void *hdr)
{
	if (0 == ppp_ipcp_hdr) throw ePPPIpcpNotFound();

	return ppp_ipcp_hdr->code;
}

inline static
void set_ipcp_code(void *hdr, uint8_t code)
{
	if (0 == ppp_ipcp_hdr) throw ePPPIpcpNotFound();

	ppp_ipcp_hdr->code = code;
}

inline static
uint8_t get_ipcp_ident(void *hdr)
{
	if (0 == ppp_ipcp_hdr) throw ePPPIpcpNotFound();

	return ppp_ipcp_hdr->ident;
}

inline static
void set_ipcp_ident(void *hdr, uint8_t ident)
{
	if (0 == ppp_ipcp_hdr) throw ePPPIpcpNotFound();

	ppp_ipcp_hdr->ident = ident;
}

inline static
uint16_t get_ipcp_length(void *hdr)
{
	if (0 == ppp_ipcp_hdr) throw ePPPIpcpNotFound();

	return CPC_BE16TOH(ppp_ipcp_hdr->length);
}

inline static
void set_ipcp_length(uint16_t len) throw (ePPPIpcpNotFound)
{
	if (0 == ppp_ipcp_hdr) throw ePPPIpcpNotFound();

	ppp_ipcp_hdr->length = CPC_HTOBE16(len);
}

inline static
fppp_ipcp_option* get_ipcp_option(void *hdr, enum ppp_ipcp_option_t option)
{
	if (ipcp_options.find(option) == ipcp_options.end()) throw ePPPIpcpOptionNotFound();

	return ipcp_options[option];
}


#endif //_CPC_PPP_H_
