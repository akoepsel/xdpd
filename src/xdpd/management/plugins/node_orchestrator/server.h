#ifndef SERVER_H_
#define SERVER_H_ 1

#pragma once

#include <json_spirit/json_spirit.h>
#include <json_spirit/writer.h>
#include <json_spirit/reader.h>
#include <json_spirit/reader_template.h>
#include <json_spirit/writer_template.h>
#include <json_spirit/value.h>


#include <string>
#include <sstream>
#include <list>

#include "node_orchestrator.h"
#include "LSI.h"
#include "sockutils.h"

#define LISTEN_ON_PORT	"2525"

using namespace std;
using namespace json_spirit;

namespace xdpd
{

class Server
{
public:
	static void *listen(void *param);

private:
	static string processCommand(string message);

/**
*	Example of command to create a new LSI
*
	{
		"command" : "create-lsi",
		"controller" :
			{
				"address" : "127.0.0.1",
				"port" : "6653"
			},
		"ports" : ["ge0","ge1"],
		"network-functions" : ["firewall"],
		"virtual-links" : ["0x100","0x101"]
	}
*/
	static string createLSI(string message);
	
/**
*	Example of command to discover the physical ports of xDPD
*
	{
		"command" : "discover-physical-ports"
	}
*
*	Example of answer
*	
	{
		"answer" : "discover-physical-ports",
		"status" : "ok",
		"ports" : ["ge0", "ge1"]
	}
*/
	static string discoverPhyPorts(string message);
};

}

#endif //SERVER_H_
