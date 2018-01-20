/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef XDPD_GNU_LINUX_XDPD_CONFIG_RSS_H
#define XDPD_GNU_LINUX_XDPD_CONFIG_RSS_H

#include <rte_config.h>
#include <rte_common.h>
#include <rte_memory.h>
#include <rte_eal.h>
#include <rte_lcore.h>
#include <rte_kni.h>

#include <rofl/datapath/pipeline/switch_port.h>

/**
* @file config.h
*
* @author Marc Sune<marc.sune (at) bisdn.de>
*
* Temporally header file to define RSS config
*/

//Auxiliary struct to hold lcore to socket mappings
struct lcore {
	int socket_id;
	int is_master;  //0:work horse, 1:master
	int is_enabled; //0:disabled, 1:enabled
	int next_lcore_id; //lcore_id of next lcore on actual socket or -1, excluding the master lcore
	int is_wk_lcore; //1: lcore is used as worker lcore (running the openflow pipeline)
	int is_rx_lcore; //1: lcore is used for conducting packet RX operations
	int is_tx_lcore; //1: lcore is used for conducting packet TX operations
	int is_svc_lcore; //1: lcore is service core
} __rte_cache_aligned;

/**
* lcores
*/
extern struct lcore lcores[RTE_MAX_LCORE];

//Auxiliary struct to hold physical port to socket mappings
struct phyport {
	int socket_id;
	int is_enabled; //0:disabled, 1:enabled, -1:administratively disabled
	unsigned int nb_rx_queues; //number of rxqueues to be used
	unsigned int nb_tx_queues; //number of txqueues to be used, (nb_rx_queues == nb_tx_queues)
	int is_vf; //0:not a virtual function, 1:virtual function
	int parent_port_id; //-1:no parent port, only valid when is_vf is true
	unsigned int nb_vfs; //number of virtual functions attached to this port, only valid when is_vf is false
	int vf_id; //vf_id for this phyport, only valid when is_vf is true
	int is_virtual; //true: port is kni, ring, ...
} __rte_cache_aligned;

/**
* phyports
*/
extern struct phyport phyports[RTE_MAX_ETHPORTS];

/**
 * virtual port names (kni, ring, ...)
 */
extern char vport_names[RTE_MAX_ETHPORTS][SWITCH_PORT_MAX_LEN_NAME];

struct virport {
	struct rte_kni_conf conf;
	struct rte_kni_ops  ops;
};

/**
* virports
*/
extern struct virport virports[RTE_MAX_ETHPORTS];

#endif //XDPD_GNU_LINUX_XDPD_CONFIG_RSS_H
