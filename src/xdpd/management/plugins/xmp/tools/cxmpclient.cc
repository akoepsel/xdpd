/*
 * cxmpclient.cc
 *
 *  Created on: 12.01.2014
 *      Author: andreas
 */

#include "cxmpclient.h"

using namespace xdpd::mgmt::protocol;

cxmpclient::cxmpclient() :
		socket(NULL),
		dest(AF_INET, "127.0.0.1", 8444),
		mem(NULL)
{
	socket = rofl::csocket::csocket_factory(rofl::csocket::SOCKET_TYPE_PLAIN, this);

	socket_params = rofl::csocket::get_default_params(rofl::csocket::SOCKET_TYPE_PLAIN);

	socket_params.set_param(rofl::csocket::PARAM_KEY_REMOTE_HOSTNAME).set_string("127.0.0.1");
	socket_params.set_param(rofl::csocket::PARAM_KEY_REMOTE_PORT).set_string("8444");
	socket_params.set_param(rofl::csocket::PARAM_KEY_DOMAIN).set_string("inet");
	socket_params.set_param(rofl::csocket::PARAM_KEY_TYPE).set_string("stream");
	socket_params.set_param(rofl::csocket::PARAM_KEY_PROTOCOL).set_string("tcp");

	socket->connect(socket_params);

}


cxmpclient::~cxmpclient()
{

}

void
cxmpclient::handle_connected(rofl::csocket& socket)
{
	rofl::logging::debug << __PRETTY_FUNCTION__ << std::endl;
	assert(this->socket == &socket);

	socket.send(mem, dest);
	register_timer(TIMER_XMPCLNT_EXIT, 1);
}

void
cxmpclient::handle_connect_refused(rofl::csocket& socket)
{
	rofl::logging::debug << __PRETTY_FUNCTION__ << std::endl;
}

void
cxmpclient::handle_connect_failed(rofl::csocket& socket)
{
	rofl::logging::debug << __PRETTY_FUNCTION__ << std::endl;
}

void
cxmpclient::handle_timeout(
		int opaque, void *data)
{
	rofl::logging::debug << __PRETTY_FUNCTION__ << std::endl;

	switch (opaque) {
	case TIMER_XMPCLNT_EXIT: {
		exit(0);
	} break;
	default: {

	};
	}
}


void
cxmpclient::port_attach(
		uint64_t dpid, std::string const& portname)
{
	cxmpmsg msg(XMP_VERSION, XMPT_REQUEST);

	msg.get_xmpies().add_ie_command().set_command(XMPIEMCT_PORT_ATTACH);
	msg.get_xmpies().add_ie_portname().set_portname(portname);
	msg.get_xmpies().add_ie_dpid().set_dpid(dpid);

	std::cerr << "[xmpclient] sending Port-Attach request:" << std::endl << msg;

	mem = new rofl::cmemory(msg.length());
	msg.pack(mem->somem(), mem->memlen());
}


void
cxmpclient::port_detach(
		uint64_t dpid, std::string const& portname)
{
	cxmpmsg msg(XMP_VERSION, XMPT_REQUEST);

	msg.get_xmpies().add_ie_command().set_command(XMPIEMCT_PORT_DETACH);
	msg.get_xmpies().add_ie_portname().set_portname(portname);
	msg.get_xmpies().add_ie_dpid().set_dpid(dpid);

	std::cerr << "[xmpclient] sending Port-Detach request:" << std::endl << msg;

	mem = new rofl::cmemory(msg.length());
	msg.pack(mem->somem(), mem->memlen());
}


void
cxmpclient::port_enable(
		std::string const& portname)
{
	cxmpmsg msg(XMP_VERSION, XMPT_REQUEST);

	msg.get_xmpies().add_ie_command().set_command(XMPIEMCT_PORT_ENABLE);
	msg.get_xmpies().add_ie_portname().set_portname(portname);

	std::cerr << "[xmpclient] sending Port-Enable request:" << std::endl << msg;

	mem = new rofl::cmemory(msg.length());
	msg.pack(mem->somem(), mem->memlen());
}


void
cxmpclient::port_disable(
		std::string const& portname)
{
	cxmpmsg msg(XMP_VERSION, XMPT_REQUEST);

	msg.get_xmpies().add_ie_command().set_command(XMPIEMCT_PORT_DISABLE);
	msg.get_xmpies().add_ie_portname().set_portname(portname);

	std::cerr << "[xmpclient] sending Port-Disable request:" << std::endl << msg;

	mem = new rofl::cmemory(msg.length());
	msg.pack(mem->somem(), mem->memlen());
}
