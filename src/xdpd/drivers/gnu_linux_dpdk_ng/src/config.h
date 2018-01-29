/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef XDPD_GNU_LINUX_DPDK_NG_CONFIG_H
#define XDPD_GNU_LINUX_DPDK_NG_CONFIG_H

/**
* @file config.h
*
* @author Marc Sune<marc.sune (at) bisdn.de>
*
* @brief Configuration file for the xDPd DPDK GNU/Linux driver. 
*
* TODO 
*/

//Driver name
#define DRIVER_NAME "[xdpd][dpdk-ng]"

//This parameter disallows the driver to schedule phyiscal ports on logical
//cores belonging to a different CPU socket than the port(PCI).
//Warning: disabling this flag can affect performance; consider using a proper
//coremask instead
#define ABORT_ON_UNMATCHED_SCHED

/* logging */
#define RTE_LOGTYPE_XDPD RTE_LOGTYPE_USER1

/*
* BG stuff
*/

//Frequency(period) of port link status updating in milliseconds
#define BG_UPDATE_PORT_LINKS_MS 400

//Frequency(period) of port stats updating in milliseconds
#define BG_UPDATE_PORT_STATS_MS 500

//Frequency(period) of handling KNI commands in milliseconds
#define BG_HANDLE_KNI_COMMANDS_MS 1000

/*
* I/O stuff
*/
//Number of output queues per interface
#define IO_IFACE_NUM_QUEUES 1 //8
#define MAX_RX_QUEUE_PER_LCORE 64
#define MAX_TX_QUEUE_PER_PORT 16 // for pf: RTE_MAX_ETHPORTS
#define MAX_RX_QUEUE_PER_PORT 128
#define IO_IFACE_MAX_PKT_BURST 32
#define IO_MAX_PACKET_SIZE 1518

/* new defines */
#define PROC_MAX_RX_QUEUES_PER_LCORE 128
#define PROC_MAX_TX_QUEUES_PER_LCORE 128
#define MAX_ETH_RX_BURST_SIZE_DEFAULT 64
#define MAX_ETH_TX_BURST_SIZE_DEFAULT 64
#define MAX_EVT_PROC_BURST_SIZE_DEFAULT 1
#define MAX_EVT_TX_BURST_SIZE_DEFAULT 64
#if 1
#define PROCESSING_MAX_PORTS_PER_CORE 32
#define PROCESSING_MAX_PORTS 128
#endif
#define PROCESSING_TXRING_DRAIN_INTERVAL_DEFAULT 100 /* in clock cycles */
#define PROCESSING_TXRING_DRAIN_THRESHOLD_DEFAULT 16
#define PROCESSING_TXRING_DRAIN_QUEUE_CAPACITY_DEFAULT 128
/* end of new defines */

//Bufferpool reservoir(PKT_INs); ideally at least X*max_num_lsis
#define IO_BUFFERPOOL_RESERVOIR 2048

//TX queue (rte_ring) size
#define IO_TX_LCORE_QUEUE_SLOTS 2<<11 //2048

//Drain timing
#define IO_BURST_TX_DRAIN_US 100 /* TX drain every ~100us */

//Buffer storage(PKT_IN) max buffers
#define IO_PKT_IN_STORAGE_MAX_BUF 2<<9 //512
//Buffer storage(PKT_IN) expiration time (seconds)
#define IO_PKT_IN_STORAGE_EXPIRATION_S 10

/*
 * RX and TX Prefetch, Host, and Write-back threshold values should be
 * carefully set for optimal performance. Consult the network
 * controller's datasheet and supporting DPDK documentation for guidance
 * on how these parameters should be set.
 */
#define RX_PTHRESH 8 /**< Default values of RX prefetch threshold reg. */
#define RX_HTHRESH 8 /**< Default values of RX host threshold reg. */
#define RX_WTHRESH 0 /**< Default values of RX write-back threshold reg. */

/*
 * These default values are optimized for use with the Intel(R) 82599 10 GbE
 * Controller and the DPDK ixgbe PMD. Consider using other values for other
 * network controllers and/or network drivers.
 */
#define TX_PTHRESH 16 /**< Default values of TX prefetch threshold reg. */
#define TX_HTHRESH 4  /**< Default values of TX host threshold reg. */
#define TX_WTHRESH 0  /**< Default values of TX write-back threshold reg. */

/*
 *  Default values for RX/TX configuration
 */

//ixgbe
#define IXGBE_MAX_RING_DESC           4096
#define IXGBE_MIN_RING_DESC           32

#define IXGBE_DEFAULT_RX_FREE_THRESH  32
#define IXGBE_DEFAULT_RX_PTHRESH      8
#define IXGBE_DEFAULT_RX_HTHRESH      8
#define IXGBE_DEFAULT_RX_WTHRESH      0

#define IXGBE_DEFAULT_TX_FREE_THRESH  32
#define IXGBE_DEFAULT_TX_PTHRESH      32
#define IXGBE_DEFAULT_TX_HTHRESH      0
#define IXGBE_DEFAULT_TX_WTHRESH      0
#define IXGBE_DEFAULT_TX_RSBIT_THRESH 32

//i40e
#define I40E_NUM_DESC_DEFAULT         512
#define	I40E_ALIGN_RING_DESC	      32

#define	I40E_MAX_RING_DESC	          4096
#define	I40E_MIN_RING_DESC	          64

#define I40E_DEFAULT_RX_FREE_THRESH   32
#define I40E_DEFAULT_RX_PTHRESH       8
#define I40E_DEFAULT_RX_HTHRESH       8
#define I40E_DEFAULT_RX_WTHRESH       0

#define I40E_DEFAULT_TX_FREE_THRESH   32
#define I40E_DEFAULT_TX_PTHRESH       32
#define I40E_DEFAULT_TX_HTHRESH       0
#define I40E_DEFAULT_TX_WTHRESH       0
#define I40E_DEFAULT_TX_RSBIT_THRESH  32


/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_RX_DESC_DEFAULT 512
#define RTE_TX_DESC_DEFAULT 512

/*
* Processing stuff
*/

//Maximum number of CPU sockets
#define NB_SOCKETS  8

//DPDK defines

/*
 * The core mask defines the DPDK core mask.
 *
 * In xDPd the core mask must always include 0x1 as the management core which
 * is reserved and WON'T do I/O
 *
 * So at least another I/O core needs to be defined. Default is Ox2 is the only
 * core doing I/O
 *
 * The coremask can be overriden using driver extra parameters. However this is
 * not recommended for stable setups
 */
#define DEFAULT_RTE_CORE_MASK 0x0000ffff

#define DEFAULT_RTE_MASTER_LCORE 0

//Other parameters
#define RTE_MEM_CHANNELS 4
#define MBUF_SIZE 16383

/* (9000 + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM) */

/*
 * Default number of MBUFs per pool
 */
#define DEFAULT_NB_MBUF 65365


/**
* Uncomment the following line to enable the semaphore and implement a sleep/
* wake mechanism between xdpd and the NF process (DPDK secondary process)
*
* NOT RECOMMENDED
*/
//#define ENABLE_DPDK_SECONDARY_SEMAPHORE

//IVANO - FIXME: write a meaningfull value
//#define PKT_TO_NF_THRESHOLD 	200


#endif //XDPD_GNU_LINUX_DPDK_NG_CONFIG_H
