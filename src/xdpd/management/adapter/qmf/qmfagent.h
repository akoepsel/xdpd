/*
 * qmfagent.h
 *
 * Copyright (C) 2013 BISDN GmbH <andi@bisdn.de>
 *
 *  Created on: 26.07.2013
 *      Author: andreas
 */

#ifndef QMFAGENT_H_
#define QMFAGENT_H_ 1

#ifdef __cplusplus
extern "C" {
#endif
#include <inttypes.h>
#ifdef __cplusplus
}
#endif

#include <map>
#include <string>
#include <ostream>
#include <exception>

#include <qpid/messaging/Connection.h>
#include <qpid/messaging/Duration.h>
#include <qmf/AgentSession.h>
#include <qmf/AgentEvent.h>
#include <qmf/Schema.h>
#include <qmf/SchemaProperty.h>
#include <qmf/SchemaMethod.h>
#include <qmf/Data.h>
#include <qmf/DataAddr.h>
#include <qmf/EventNotifier.h>
#include <qpid/types/Variant.h>

#include <rofl/common/ciosrv.h>
#include <rofl/common/crofbase.h>

#include "../../switch_manager.h"
#include "../../port_manager.h"

namespace rofl
{

class eQmfAgentBase 		: public std::exception {};
class eQmfAgentInval		: public eQmfAgentBase {};
class eQmfAgentInvalSubcmd	: public eQmfAgentInval {};

class qmfagent :
		public rofl::ciosrv
{
	static qmfagent 				*qmf_agent;
	qmfagent(std::string const& broker_url = std::string("127.0.0.1"));
	qmfagent(qmfagent const& agent);
	~qmfagent();

private:

	std::string						broker_url;

	qpid::messaging::Connection 	connection;
	qmf::AgentSession 				session;
	qmf::posix::EventNotifier		notifier;
	std::string						qmf_package;
	qmf::Schema						sch_exception;
	qmf::Schema						sch_xdpd;

	struct qmf_data_t {
		qmf::Data 					data;
		qmf::DataAddr 				addr;
	};

	std::map<uint64_t, struct qmf_data_t>	qnodes;
	std::map<uint64_t, std::map<int, struct qmf_data_t> >	qlinks;
	std::map<uint64_t, std::map<int, std::map<int, struct qmf_data_t> > > qaddrs;
	std::map<uint64_t, std::map<int, struct qmf_data_t> >	qbearers;
	std::map<uint64_t, std::map<int, struct qmf_data_t> > 	qroutes;
	std::map<uint64_t, std::map<int, std::map<int, struct qmf_data_t> > > qnexthops;
	std::map<uint64_t, std::map<int, struct qmf_data_t> >	qneighs;

	enum addr_cmd_t {
		ADDR_ADD = 1,
		ADDR_DEL = 2,
	};

public:

	/**
	 *
	 */
	static qmfagent&
	get_instance(std::string const& broker_url = std::string("127.0.0.1"));

protected:

	/**
	 *
	 */
	virtual void
	handle_timeout(int opaque);

	/**
	 *
	 */
	virtual void
	handle_revent(int fd);

private:

	/**
	 *
	 */
	void
	set_qmf_schema();

	/**
	 *
	 */
	bool
	method(qmf::AgentEvent& event);

	/**
	 *
	 */
	bool
	methodLsiCreate(qmf::AgentEvent& event);

	/**
	 *
	 */
	bool
	methodLsiDestroy(qmf::AgentEvent& event);

	/**
	 *
	 */
	void
	create_switch();

	/**
	 *
	 */
	void
	destroy_switch();
};

}; // end of namespace

#endif /* QMFAGENT_H_ */
