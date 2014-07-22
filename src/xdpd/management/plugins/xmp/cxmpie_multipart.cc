/*
 * cxmpie_multipart.cc
 *
 *  Created on: Jul 15, 2014
 *      Author: tobi
 */

#include "cxmpie_multipart.h"

#include "cxmpie_command.h"
#include "cxmpie_dpid.h"
#include "cxmpie_portname.h"
#include "xdpd_mgmt_protocol.h"

namespace xdpd {
namespace mgmt {
namespace protocol {

cxmpie_multipart::cxmpie_multipart() :
		cxmpie(XMPIET_MULTIPART, sizeof(struct xmp_ie_header_t))
{
}

cxmpie_multipart::cxmpie_multipart(cxmpie_multipart const &elem) :
		cxmpie(elem)
{
	copy_ies(elem.ies);
}

cxmpie_multipart::cxmpie_multipart(cxmpie const& elem) :
		cxmpie(elem)
{
	if (XMPIET_MULTIPART != elem.get_type()) throw eXmpIeInval();

	copy_ies(static_cast<cxmpie_multipart const &>(elem).ies);
}

cxmpie_multipart&
cxmpie_multipart::operator =(cxmpie_multipart const &rhs)
{
	assert(XMPIET_MULTIPART == rhs.get_type());

	if (this == &rhs) return *this;

	copy_ies(rhs.ies);

	return *this;
}

cxmpie_multipart::~cxmpie_multipart()
{
	clear();
}

size_t
cxmpie_multipart::length() const
{
	size_t len = sizeof(struct xmp_header_t);
	for (std::deque<cxmpie*>::const_iterator it = ies.begin(); it != ies.end();
			++it) {
		len += (*it)->length();
	}
	return len;
}

void
cxmpie_multipart::pack(uint8_t* buf, size_t buflen)
{
	if (buflen < length()) throw eXmpIeInval();
	for (std::deque<cxmpie*>::const_iterator it = ies.begin(); it != ies.end();
			++it) {
		assert(buflen >= 0);
		(*it)->pack(buf, (*it)->length());
		buf += (*it)->length();
		buflen -= (*it)->length();
	}
}

void
cxmpie_multipart::unpack(uint8_t* buf, size_t buflen)
{
	clear();

	while (buflen > sizeof(struct xmp_ie_header_t)) {
		struct xmp_ie_header_t *hdr = (struct xmp_ie_header_t*) buf;
		if ((be16toh(hdr->len) > buflen)
				|| (be16toh(hdr->len) < sizeof(struct xmp_ie_header_t))) {
			rofl::logging::error
					<< "[xdpd][xmp] unpacking IE list, invalid length field in IE"
					<< std::endl;
			return;
		}
		cxmpie tmp(buf, be16toh(hdr->len));
		ies.push_back(bakery(&tmp));
		buf += be16toh(hdr->len);
		buflen -= be16toh(hdr->len);
	}
}

void
cxmpie_multipart::push_back(cxmpie* ie)
{
	ies.push_back(ie);
}

void
cxmpie_multipart::copy_ies(std::deque<cxmpie*> const &rhs)
{
	clear();

	for (std::deque<cxmpie*>::const_iterator it = rhs.begin(); it != rhs.end();
			++it) {
		ies.push_back(bakery(*it));
	}
}

cxmpie*
cxmpie_multipart::bakery(cxmpie *ref)
{
	switch (ref->get_type()) {

	case XMPIET_NONE:
		break;
	case XMPIET_COMMAND:
		return new cxmpie_command(*ref);
		break;
	case XMPIET_PORTNAME:
		return new cxmpie_portname(*ref);
		break;
	case XMPIET_DPID:
		return new cxmpie_dpid(*ref);
		break;
	case XMPIET_MULTIPART:
		// todo do we wanna have multipart in multipart?
	default:
		assert(0);
		break;
	}

	return NULL;
}

void
cxmpie_multipart::clear()
{
	while (not ies.empty()) {
		cxmpie* tmp = ies.front();
		ies.pop_front();
		delete tmp;
	}
}

} /* namespace protocol */
} /* namespace mgmt */
} /* namespace xdpd */
