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
#include "orchestrator_constants.h"

using namespace std;
using namespace json_spirit;

namespace xdpd
{

class MessageHandler
{

friend class NodeOrchestrator;

protected:
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
		"network-functions" : [
			 {
		        "name" : "VPN",
		        "type" : "dpdk",
		        "ports" : [
		            "VPN_1"
		        ]
		    },
		    {
		        "name" : "firewall",
		        "type" : "docker"
		        "ports" : [
		            "firewall_1",
		            "firewall_2",
		            "firewall_3"
		        ]
		    }	
		],
		"virtual-links" : 
			{
				"number" : "2",
				"remote-lsi" : "1"
			}
	}
*
*	Example of answer
*
	{
		"command" : "create-lsi",
		"status" : "ok",
		"lsi-id" : "0x200",
		"ports" : [
			 { 
			 	"name" : "ge0",
			 	"id" : "0x1"
			 },
			{
				"name" : "ge1",
				"id" : "0x2"
			}	
		],
		"network-functions" : [
			{
		        "name" : "VPN",
		        "ports" : [
		            {
		                "id" : 1,
		                "name" : "VPN_1"
		            }
			    ]
		    },
		    {
		        "name" : "firewall",
		        "ports" : [
		            {
		                "id" : 2,
		                "name" : "firewall_1"
		            },
		            {
		                "id" : 3,
		                "name" : "firewall_2"
		            },
		            {
		                "id" : 4,
		                "name" : "firewall_3"
		            }
		        ]
		    }
		],
		"virtual-links" : [
			{
				"local-id" : "0x4",
				"remote-id" : "0xFA",	
			},
			{
				"local-id" : "0x5",
				"remote-id" : "0xFB",	
			}
		]
	}
*/
	static string createLSI(string message);
	static string createLSIAnswer(LSI lsi, map<string,map<string,uint32_t> > nfPorts,list<pair<unsigned int, unsigned int> > virtual_links);

/**
*	Example of command to create a new LSI
*
	{
		"command" : "create-lsi",
		"lsi-id" : "0x200"
	}
*
*	Example of answer
*
	{
		"command" : "destroy-lsi",
		"status" : "ok"
*/
	static string destroyLSI(string message);

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

/**	
*	Example of answer
*	
	{
		"answer" : "create-lsi",
		"status" : "error",
		"message" : "bla bla bla"
	}
*/
	static string createErrorMessage(string command, string message);
};

}

#endif //SERVER_H_
