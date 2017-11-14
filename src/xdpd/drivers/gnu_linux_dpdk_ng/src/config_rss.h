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
};

/**
* lcores
*/
extern struct lcore lcores[RTE_MAX_LCORE];

//Auxiliary struct to hold physical port to socket mappings
struct phyport {
	int socket_id;
	int is_enabled; //0:disabled, 1:enabled, -1:administratively disabled
	int nb_rx_queues; //number of rxqueues to be used
	int nb_tx_queues; //number of txqueues to be used, (nb_rx_queues == nb_tx_queues)
};

/**
* phyports
*/
extern struct phyport phyports[RTE_MAX_ETHPORTS];

#define LCORE_PARAMS_MAX 1024

//Auxiliary struct to hold binding between port, queue and lcore
struct lcore_params {
	uint8_t lsi_id;
	uint8_t port_id;
	uint8_t queue_id;
	uint8_t lcore_id;
} __rte_cache_aligned;

/**
* lcore parameters (RSS)
*/
extern struct lcore_params lcore_params[LCORE_PARAMS_MAX];

/**
* lcore number of parameters
*/
extern uint16_t nb_lcore_params;

#endif //XDPD_GNU_LINUX_XDPD_CONFIG_RSS_H
