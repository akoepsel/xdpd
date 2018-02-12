#include "iface_manager.h"
#include "../processing/mem_manager.h"
#include <math.h>
#include <rofl/datapath/hal/cmm.h>
#include <utils/c_logger.h>
#include <rofl/datapath/pipeline/openflow/of_switch.h>
#include <rofl/datapath/pipeline/common/datapacket.h>
#include <rofl/datapath/pipeline/physical_switch.h>

#include "port_state.h"

#include <assert.h> 
extern "C" {
#include <rte_config.h> 
#include <rte_common.h> 
#include <rte_malloc.h> 
#include <rte_errno.h> 
#include <rte_eth_ctrl.h>
#include <rte_bus_pci.h>
#include <rte_ethdev.h>
#include <rte_bus_vdev.h>
#include <rte_cycles.h>
#include <rte_eth_ring.h>

#ifdef RTE_LIBRTE_IXGBE_PMD
#include <rte_pmd_ixgbe.h>
#endif
#ifdef RTE_LIBRTE_I40E_PMD
#include <rte_pmd_i40e.h>
#endif
}

#include <yaml-cpp/yaml.h>
extern YAML::Node y_config_dpdk_ng;

#include <fcntl.h>
#include <set>
#include <map>
#include <algorithm>

#define DPDK_DRIVER_NAME_I40E_PF "net_i40e"
#define DPDK_DRIVER_NAME_I40E_VF "net_i40e_vf"
#define DPDK_DRIVER_NAME_IXGBE_PF "net_ixgbe"
#define DPDK_DRIVER_NAME_IXGBE_VF "net_ixgbe_vf"

#if 0
#define NB_MBUF                                                                                                        \
	RTE_MAX((nb_ports * nb_rx_queue * RTE_RX_DESC_DEFAULT + nb_ports * nb_lcores * IO_IFACE_MAX_PKT_BURST +        \
		 nb_ports * n_tx_queue * RTE_TX_DESC_DEFAULT + nb_lcores * MEMPOOL_CACHE_SIZE),                        \
		(unsigned)8192)

#define MEMPOOL_CACHE_SIZE 256

#define VLAN_RX_FILTER
//#define VLAN_SET_MACVLAN_FILTER
//#define USE_INPUT_FILTER_SET

struct ether_addr ports_eth_addr[RTE_MAX_ETHPORTS];

/* Static global variables used within this file. */
//static uint16_t nb_rxd = RTE_RX_DESC_DEFAULT;
//uint16_t nb_txd = RTE_TX_DESC_DEFAULT;
//uint16_t nb_rxd = RTE_RX_DESC_DEFAULT;
#endif

switch_port_t* phy_port_mapping[PORT_MANAGER_MAX_PORTS] = {0};
uint8_t nb_phy_ports = 0;
pthread_rwlock_t iface_manager_rwlock = PTHREAD_RWLOCK_INITIALIZER;

//a set of available NUMA sockets (socket_id)
extern std::set<int> numa_nodes;

/* a map of available RX logical cores per NUMA socket (set of lcore_id) */
extern std::map<unsigned int, std::set<unsigned int> > rx_lcores;
/* a map of available TX logical cores per NUMA socket (set of lcore_id) */
extern std::map<unsigned int, std::set<unsigned int> > tx_lcores;

extern unsigned int mem_pool_size;
extern unsigned int mbuf_dataroom;
extern unsigned int dpdk_memory_mempool_direct_cache_size;
extern unsigned int dpdk_memory_mempool_direct_priv_size;
extern unsigned int dpdk_memory_mempool_indirect_cache_size;
extern unsigned int dpdk_memory_mempool_indirect_priv_size;

/* Shinae Woo and KyoungSoo Park
 * "Scalable TCP Session Monitoring with Symmetric Receive-side Scaling"
 */
static uint8_t sym_rss_hash_key[] = {
		0x6D, 0x5A, 0x6D, 0x5A,
		0x6D, 0x5A, 0x6D, 0x5A,
		0x6D, 0x5A, 0x6D, 0x5A,
		0x6D, 0x5A, 0x6D, 0x5A,
		0x6D, 0x5A, 0x6D, 0x5A,
		0x6D, 0x5A, 0x6D, 0x5A,
		0x6D, 0x5A, 0x6D, 0x5A,
		0x6D, 0x5A, 0x6D, 0x5A,
		0x6D, 0x5A, 0x6D, 0x5A,
		0x6D, 0x5A, 0x6D, 0x5A,
};


static int set_vf_vlan_filter(uint16_t port_id, uint16_t vlan_id, uint64_t vf_mask, uint8_t on)
{
	struct rte_eth_dev_info dev_info;
	rte_eth_dev_info_get(port_id, &dev_info);

#ifdef RTE_LIBRTE_IXGBE_PMD
	if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_IXGBE_PF, sizeof(DPDK_DRIVER_NAME_IXGBE_PF)) == 0){
		return rte_pmd_ixgbe_set_vf_vlan_filter(port_id, vlan_id, vf_mask, on);
	}
#endif

#ifdef RTE_LIBRTE_I40E_PMD
	if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_I40E_PF, sizeof(DPDK_DRIVER_NAME_I40E_PF)) == 0){
		return rte_pmd_i40e_set_vf_vlan_filter(port_id, vlan_id, vf_mask, on);
	}
#endif

	XDPD_ERR(DRIVER_NAME" iface_manager::set_vf_vlan_filter() not implemented for devices of type: %s\n", dev_info.driver_name);
	return -ENOTSUP;
}

static void set_promisc(uint16_t port_id, uint8_t on)
{
	if (on) {
		rte_eth_promiscuous_enable(port_id);
	} else {
		rte_eth_promiscuous_disable(port_id);
	}
}

static void set_allmulticast(uint16_t port_id, uint8_t on)
{
	if (on) {
		rte_eth_allmulticast_enable(port_id);
	} else {
		rte_eth_allmulticast_disable(port_id);
	}
}

static int set_tx_loopback(uint8_t port_id, uint8_t on)
{
	struct rte_eth_dev_info dev_info;
	rte_eth_dev_info_get(port_id, &dev_info);

#ifdef RTE_LIBRTE_IXGBE_PMD
	if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_IXGBE_PF, sizeof(DPDK_DRIVER_NAME_IXGBE_PF)) == 0){
		return rte_pmd_ixgbe_set_tx_loopback(port_id, on);
	}
#endif

#ifdef RTE_LIBRTE_I40E_PMD
	if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_I40E_PF, sizeof(DPDK_DRIVER_NAME_I40E_PF)) == 0){
		return rte_pmd_i40e_set_tx_loopback(port_id, on);
	}
#endif

	XDPD_ERR(DRIVER_NAME" iface_manager::set_tx_loopback() not implemented for devices of type: %s\n", dev_info.driver_name);
	return -ENOTSUP;
}

#ifdef VLAN_SET_MACVLAN_FILTER
static int set_mac_vlan_filter(uint8_t port_id, struct ether_addr *address, const char *filter_type, int is_on)
{
	int ret = -EINVAL;
	struct rte_eth_mac_filter filter;

	if (!filter_type) {
		return -EINVAL;
	}

	memset(&filter, 0, sizeof(struct rte_eth_mac_filter));

	(void)rte_memcpy(&filter.mac_addr, &address, ETHER_ADDR_LEN);

	/* set VF MAC filter */
	filter.is_vf = 0;

	/* no VF ID as this is a physical device */
	filter.dst_id = 0;

	if (!strcmp(filter_type, "exact-mac"))
		filter.filter_type = RTE_MAC_PERFECT_MATCH;
	else if (!strcmp(filter_type, "exact-mac-vlan"))
		filter.filter_type = RTE_MACVLAN_PERFECT_MATCH;
	else if (!strcmp(filter_type, "hashmac"))
		filter.filter_type = RTE_MAC_HASH_MATCH;
	else if (!strcmp(filter_type, "hashmac-vlan"))
		filter.filter_type = RTE_MACVLAN_HASH_MATCH;
	else {
		printf("bad filter type");
		return ret;
	}

	if (is_on)
		ret = rte_eth_dev_filter_ctrl(port_id, RTE_ETH_FILTER_MACVLAN, RTE_ETH_FILTER_ADD, &filter);
	else
		ret = rte_eth_dev_filter_ctrl(port_id, RTE_ETH_FILTER_MACVLAN, RTE_ETH_FILTER_DELETE, &filter);

	if (ret < 0)
		printf("bad set MAC hash parameter, return code = %d\n", ret);

	return ret;
}
#endif

static int set_vf_mac_addr(uint8_t port_id, uint16_t vf_id, struct ether_addr *mac_addr)
{
	struct rte_eth_dev_info dev_info;
	rte_eth_dev_info_get(port_id, &dev_info);

#ifdef RTE_LIBRTE_IXGBE_PMD
	if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_IXGBE_PF, sizeof(DPDK_DRIVER_NAME_IXGBE_PF)) == 0){
		return rte_pmd_ixgbe_set_vf_mac_addr(port_id, vf_id, mac_addr);
	}
#endif

#ifdef RTE_LIBRTE_I40E_PMD
	if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_I40E_PF, sizeof(DPDK_DRIVER_NAME_I40E_PF)) == 0){
		return rte_pmd_i40e_set_vf_mac_addr(port_id, vf_id, mac_addr);
	}
#endif

	XDPD_ERR(DRIVER_NAME" iface_manager::set_vf_mac_addr() not implemented for devices of type: %s\n", dev_info.driver_name);
	return -ENOTSUP;
}

static int add_vf_mac_addr(uint8_t port_id, uint16_t vf_id, struct ether_addr *mac_addr)
{
	struct rte_eth_dev_info dev_info;
	rte_eth_dev_info_get(port_id, &dev_info);

#ifdef RTE_LIBRTE_I40E_PMD
	if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_I40E_PF, sizeof(DPDK_DRIVER_NAME_I40E_PF)) == 0){
		return rte_pmd_i40e_add_vf_mac_addr(port_id, vf_id, mac_addr);
	}
#endif

	XDPD_ERR(DRIVER_NAME" iface_manager::set_vf_mac_addr() not implemented for devices of type: %s\n", dev_info.driver_name);
	return -ENOTSUP;
}

static int set_vf_mac_anti_spoof(uint8_t port_id, uint16_t vf_id, uint8_t on)
{
	struct rte_eth_dev_info dev_info;
	rte_eth_dev_info_get(port_id, &dev_info);

#ifdef RTE_LIBRTE_IXGBE_PMD
	if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_IXGBE_PF, sizeof(DPDK_DRIVER_NAME_IXGBE_PF)) == 0){
		return rte_pmd_ixgbe_set_vf_mac_anti_spoof(port_id, vf_id, on);
	}
#endif

#ifdef RTE_LIBRTE_I40E_PMD
	if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_I40E_PF, sizeof(DPDK_DRIVER_NAME_I40E_PF)) == 0){
		return rte_pmd_i40e_set_vf_mac_anti_spoof(port_id, vf_id, on);
	}
#endif

	XDPD_ERR(DRIVER_NAME" iface_manager::set_vf_mac_anti_spoof() not implemented for devices of type: %s\n", dev_info.driver_name);
	return -ENOTSUP;
}

static int set_vf_vlan_anti_spoof(uint8_t port_id, uint16_t vf_id, uint8_t on)
{
	struct rte_eth_dev_info dev_info;
	rte_eth_dev_info_get(port_id, &dev_info);

#ifdef RTE_LIBRTE_IXGBE_PMD
	if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_IXGBE_PF, sizeof(DPDK_DRIVER_NAME_IXGBE_PF)) == 0){
		return rte_pmd_ixgbe_set_vf_vlan_anti_spoof(port_id, vf_id, on);
	}
#endif

#ifdef RTE_LIBRTE_I40E_PMD
	if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_I40E_PF, sizeof(DPDK_DRIVER_NAME_I40E_PF)) == 0){
		return rte_pmd_i40e_set_vf_vlan_anti_spoof(port_id, vf_id, on);
	}
#endif

	XDPD_ERR(DRIVER_NAME" iface_manager::set_vf_vlan_anti_spoof() not implemented for devices of type: %s\n", dev_info.driver_name);
	return -ENOTSUP;
}

static int set_vf_unicast_promisc(uint8_t port_id, uint16_t vf_id, uint8_t on)
{
	struct rte_eth_dev_info dev_info;
	rte_eth_dev_info_get(port_id, &dev_info);

#ifdef RTE_LIBRTE_I40E_PMD
	if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_I40E_PF, sizeof(DPDK_DRIVER_NAME_I40E_PF)) == 0){
		return rte_pmd_i40e_set_vf_unicast_promisc(port_id, vf_id, on);
	}
#endif

	XDPD_ERR(DRIVER_NAME" iface_manager::set_vf_unicast_promisc() not implemented for devices of type: %s\n", dev_info.driver_name);
	return -ENOTSUP;
}

static int set_vf_multicast_promisc(uint8_t port_id, uint16_t vf_id, uint8_t on)
{
	struct rte_eth_dev_info dev_info;
	rte_eth_dev_info_get(port_id, &dev_info);

#ifdef RTE_LIBRTE_I40E_PMD
	if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_I40E_PF, sizeof(DPDK_DRIVER_NAME_I40E_PF)) == 0){
		return rte_pmd_i40e_set_vf_multicast_promisc(port_id, vf_id, on);
	}
#endif

	XDPD_ERR(DRIVER_NAME" iface_manager::set_vf_multicast_promisc() not implemented for devices of type: %s\n", dev_info.driver_name);
	return -ENOTSUP;
}

static int set_vf_broadcast(uint8_t port_id, uint16_t vf_id, uint8_t on)
{
	struct rte_eth_dev_info dev_info;
	rte_eth_dev_info_get(port_id, &dev_info);

#ifdef RTE_LIBRTE_I40E_PMD
	if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_I40E_PF, sizeof(DPDK_DRIVER_NAME_I40E_PF)) == 0){
		return rte_pmd_i40e_set_vf_broadcast(port_id, vf_id, on);
	}
#endif

	XDPD_ERR(DRIVER_NAME" iface_manager::set_vf_broadcast() not implemented for devices of type: %s\n", dev_info.driver_name);
	return -ENOTSUP;
}

static int set_vf_vlan_insert(uint8_t port_id, uint16_t vf_id, uint16_t vlan_id)
{
	struct rte_eth_dev_info dev_info;
	rte_eth_dev_info_get(port_id, &dev_info);

#ifdef RTE_LIBRTE_IXGBE_PMD
	if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_IXGBE_PF, sizeof(DPDK_DRIVER_NAME_IXGBE_PF)) == 0){
		return rte_pmd_ixgbe_set_vf_vlan_insert(port_id, vf_id, vlan_id);
	}
#endif

#ifdef RTE_LIBRTE_I40E_PMD
	if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_I40E_PF, sizeof(DPDK_DRIVER_NAME_I40E_PF)) == 0){
		return rte_pmd_i40e_set_vf_vlan_insert(port_id, vf_id, vlan_id);
	}
#endif

	XDPD_ERR(DRIVER_NAME" iface_manager::set_vf_vlan_insert() not implemented for devices of type: %s\n", dev_info.driver_name);
	return -ENOTSUP;
}

static int set_vf_vlan_stripq(uint8_t port_id, uint16_t vf_id, uint8_t on)
{
	struct rte_eth_dev_info dev_info;
	rte_eth_dev_info_get(port_id, &dev_info);

#ifdef RTE_LIBRTE_IXGBE_PMD
	if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_IXGBE_PF, sizeof(DPDK_DRIVER_NAME_IXGBE_PF)) == 0){
		return rte_pmd_ixgbe_set_vf_vlan_stripq(port_id, vf_id, on);
	}
#endif

#ifdef RTE_LIBRTE_I40E_PMD
	if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_I40E_PF, sizeof(DPDK_DRIVER_NAME_I40E_PF)) == 0){
		return rte_pmd_i40e_set_vf_vlan_stripq(port_id, vf_id, on);
	}
#endif

	XDPD_ERR(DRIVER_NAME" iface_manager::set_vf_vlan_stripq() not implemented for devices of type: %s\n", dev_info.driver_name);
	return -ENOTSUP;
}

static int set_vf_vlan_tag(uint8_t port_id, uint16_t vf_id, uint8_t on)
{
	struct rte_eth_dev_info dev_info;
	rte_eth_dev_info_get(port_id, &dev_info);

#ifdef RTE_LIBRTE_I40E_PMD
	if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_I40E_PF, sizeof(DPDK_DRIVER_NAME_I40E_PF)) == 0){
		return rte_pmd_i40e_set_vf_vlan_tag(port_id, vf_id, on);
	}
#endif

	XDPD_ERR(DRIVER_NAME" iface_manager::set_vf_vlan_tag() not implemented for devices of type: %s\n", dev_info.driver_name);
	return -ENOTSUP;
}

#ifdef VLAN_SET_MACVLAN_FILTER
static int set_vf_mac_vlan_filter(uint8_t port_id, uint8_t vf_id, struct ether_addr *address, const char *filter_type, int is_on)
{
	int ret = -EINVAL;
	struct rte_eth_mac_filter filter;

	if (!filter_type) {
		return -EINVAL;
	}

	memset(&filter, 0, sizeof(struct rte_eth_mac_filter));

	(void)rte_memcpy(&filter.mac_addr, &address, ETHER_ADDR_LEN);

	/* set VF MAC filter */
	filter.is_vf = 1;

	/* set VF ID */
	filter.dst_id = vf_id;

	if (!strcmp(filter_type, "exact-mac"))
		filter.filter_type = RTE_MAC_PERFECT_MATCH;
	else if (!strcmp(filter_type, "exact-mac-vlan"))
		filter.filter_type = RTE_MACVLAN_PERFECT_MATCH;
	else if (!strcmp(filter_type, "hashmac"))
		filter.filter_type = RTE_MAC_HASH_MATCH;
	else if (!strcmp(filter_type, "hashmac-vlan"))
		filter.filter_type = RTE_MACVLAN_HASH_MATCH;
	else {
		printf("bad filter type");
		return ret;
	}

	if (is_on)
		ret = rte_eth_dev_filter_ctrl(port_id, RTE_ETH_FILTER_MACVLAN, RTE_ETH_FILTER_ADD, &filter);
	else
		ret = rte_eth_dev_filter_ctrl(port_id, RTE_ETH_FILTER_MACVLAN, RTE_ETH_FILTER_DELETE, &filter);

	if (ret < 0)
		printf("bad set MAC hash parameter, return code = %d\n", ret);

	return ret;
}
#endif

















#ifdef USE_INPUT_FILTER_SET
static int set_hash_input_set(uint8_t port_id, enum rte_filter_input_set_op op, uint16_t type,
		      enum rte_eth_input_set_field inset)
{
	struct rte_eth_hash_filter_info info;

	memset(&info, 0, sizeof(info));
	info.info_type = RTE_ETH_HASH_FILTER_INPUT_SET_SELECT;
	info.info.input_set_conf.flow_type = type;
	info.info.input_set_conf.field[0] = inset;
	info.info.input_set_conf.inset_size = 1;
	info.info.input_set_conf.op = op;

	return rte_eth_dev_filter_ctrl(port_id, RTE_ETH_FILTER_HASH, RTE_ETH_FILTER_SET, &info);
}

static int set_hash_global_config(uint8_t port_id, enum rte_eth_hash_function hash_func, uint32_t type, int enable)
{
	struct rte_eth_hash_filter_info info;
	uint32_t idx, offset;
	int ret;

	if ((ret = rte_eth_dev_filter_supported(port_id, RTE_ETH_FILTER_HASH)) < 0) {
		printf("RTE_ETH_FILTER_HASH not supported on port %d\n", port_id);
		return ret;
	}

	memset(&info, 0, sizeof(info));
	info.info_type = RTE_ETH_HASH_FILTER_GLOBAL_CONFIG;
	info.info.global_conf.hash_func = hash_func;

	idx = type / (CHAR_BIT * sizeof(uint32_t));
	offset = type % (CHAR_BIT * sizeof(uint32_t));
	info.info.global_conf.valid_bit_mask[idx] |= (1UL << offset);

	if (enable)
		info.info.global_conf.sym_hash_enable_mask[idx] |= (1UL << offset);

	return rte_eth_dev_filter_ctrl(port_id, RTE_ETH_FILTER_HASH, RTE_ETH_FILTER_SET, &info);
}
#endif



void print_ethaddr(const char *name, const struct ether_addr *eth_addr)
{
	char buf[ETHER_ADDR_FMT_SIZE];
	ether_format_addr(buf, ETHER_ADDR_FMT_SIZE, eth_addr);
	XDPD_INFO("%s%s", name, buf);
}



rofl_result_t iface_manager_reset_port(switch_port_t *port)
{
	int ret;

	//Recover the platform state
	dpdk_port_state_t *ps = (dpdk_port_state_t *)port->platform_port_state;

	XDPD_INFO(DRIVER_NAME"[iface_manager] resetting port %u (%s)\n", ps->port_id, port->name);

	if ((ret = rte_eth_dev_reset(ps->port_id)) < 0) {
		XDPD_ERR(DRIVER_NAME"[iface_manager] Cannot reset port %u (%s) %s\n", ps->port_id, port->name, rte_strerror(-ret));
		return ROFL_FAILURE;
	}

	return ROFL_SUCCESS;
}

rofl_result_t iface_manager_start_port(switch_port_t *port)
{
	unsigned int i;
	int ret;

	if (port->type != PORT_TYPE_PHYSICAL)
		return ROFL_SUCCESS;

	//Recover the platform state
	dpdk_port_state_t *ps = (dpdk_port_state_t *)port->platform_port_state;

	XDPD_INFO(DRIVER_NAME"[iface_manager] starting port %u (%s)\n", ps->port_id, port->name);

	//Start port
	i = 0;
START_RETRY:
	if((ret=rte_eth_dev_start(ps->port_id)) < 0){
		XDPD_ERR(DRIVER_NAME"[iface_manager] Cannot start port %u (%s) %s\n", ps->port_id, port->name, rte_strerror(-ret));
		switch (ret) {
		case -ENOMEM: {

		} break;
		case -EINVAL: {

		} break;
		case -ENOTSUP: {

		} break;
		default: {
			if(++i != 100) {
				// Circumvent DPDK issues with rte_eth_dev_start
				usleep(300*1000);
				goto START_RETRY;
			}
		};
		}

		XDPD_ERR(DRIVER_NAME"[iface_manager] Cannot start port %u (%s) %s\n", ps->port_id, port->name, rte_strerror(ret));
		assert(0 && "rte_eth_dev_start failed");
		return ROFL_FAILURE; 
	}

	//Set pipeline state to UP
	if(likely(phy_port_mapping[ps->port_id]!=NULL)){
		phy_port_mapping[ps->port_id]->up = true;
	}
	
	//Reset stats
	rte_eth_stats_reset(ps->port_id);

	//Make sure the link is up
	rte_eth_dev_set_link_down(ps->port_id);
	rte_eth_dev_set_link_up(ps->port_id);

	//Set as queues setup
	ps->queues_set=true;

	int socket_id = rte_eth_dev_socket_id(ps->port_id);

	//Inform running RX tasks
	for (auto lcore_id : rx_lcores[socket_id]) {
		for (unsigned int i = 0; i < rx_core_tasks[lcore_id].nb_rx_queues; i++) {
			if (rx_core_tasks[lcore_id].rx_queues[i].port_id == ps->port_id) {
				rx_core_tasks[lcore_id].rx_queues[i].up = true;
				XDPD_INFO(DRIVER_NAME"[processing][tasks][rx] rx-task-%u.%02u: enabling port %u (%u)\n",
						socket_id, lcore_id, ps->port_id, rx_core_tasks[lcore_id].rx_queues[i].up);
			}
		}
	}

	//Inform running TX tasks
	for (auto lcore_id : tx_lcores[socket_id]) {
		tx_core_tasks[lcore_id].tx_queues[ps->port_id].up = true;
		XDPD_INFO(DRIVER_NAME"[processing][tasks][tx] tx-task-%u.%02u: enabling port %u (%u)\n",
				socket_id, lcore_id, ps->port_id, tx_core_tasks[lcore_id].tx_queues[ps->port_id].up);
	}

	XDPD_INFO(DRIVER_NAME"[iface_manager] port %u (%s) successfully started\n", ps->port_id, port->name);
	
	return ROFL_SUCCESS;
}

rofl_result_t iface_manager_stop_port(switch_port_t *port)
{
	if (port->type != PORT_TYPE_PHYSICAL)
		return ROFL_SUCCESS;

	//Recover the platform state
	dpdk_port_state_t *ps = (dpdk_port_state_t *)port->platform_port_state;

	XDPD_INFO(DRIVER_NAME"[iface_manager] stopping port %u (%s)\n", ps->port_id, port->name);

	int socket_id = rte_eth_dev_socket_id(ps->port_id);

	//Inform running RX tasks
	for (auto lcore_id : rx_lcores[socket_id]) {
		for (unsigned int i = 0; i < rx_core_tasks[lcore_id].nb_rx_queues; i++) {
			if (rx_core_tasks[lcore_id].rx_queues[i].port_id == ps->port_id) {
				rx_core_tasks[lcore_id].rx_queues[i].up = false;
				XDPD_INFO(DRIVER_NAME"[processing][tasks][rx] rx-task-%u.%02u: disabling port %u (%u)\n",
						socket_id, lcore_id, ps->port_id, rx_core_tasks[lcore_id].rx_queues[i].up);
			}
		}
	}

	//Inform running TX tasks
	for (auto lcore_id : tx_lcores[socket_id]) {
		tx_core_tasks[lcore_id].tx_queues[ps->port_id].up = false;
		XDPD_INFO(DRIVER_NAME"[processing][tasks][tx] tx-task-%u.%02u: disabling port %u (%u)\n",
				socket_id, lcore_id, ps->port_id, tx_core_tasks[lcore_id].tx_queues[ps->port_id].up);
	}

	//Make sure the link is down
	rte_eth_dev_set_link_down(ps->port_id);

	//Stop port
	rte_eth_dev_stop(ps->port_id);

	//Set pipeline state to UP
	if(likely(phy_port_mapping[ps->port_id]!=NULL)){
		phy_port_mapping[ps->port_id]->up = false;
	}

	XDPD_INFO(DRIVER_NAME"[iface_manager] port %u (%s) successfully stopped\n", ps->port_id, port->name);

	return ROFL_SUCCESS;
}


/**
* Returns YAML::Node for device identified by PCI address
*/
YAML::Node iface_manager_port_conf(const std::string& pci_address){
	if (y_config_dpdk_ng["dpdk"]["interfaces"].IsMap()) {
		for (auto it : y_config_dpdk_ng["dpdk"]["interfaces"]) {
			if (it.second["pci_address"].as<std::string>() == pci_address) {
				std::string ifname(it.first.as<std::string>());
				y_config_dpdk_ng["dpdk"]["interfaces"][ifname]["ifname"] = ifname; //insert copy of key 'ifname' into YAML::Node
				return y_config_dpdk_ng["dpdk"]["interfaces"][ifname];
			}
		}
	}
	throw YAML::InvalidNode();
}

/**
*
*/
bool iface_manager_port_exists(const std::string& pci_address){
	try {
		if (iface_manager_port_conf(pci_address)){
			return true;
		}
	} catch (YAML::Exception& e) {}
	return false;
}

/**
*
*/
bool iface_manager_port_setting_exists(const std::string& pci_address, const std::string& key){
	try {
		return iface_manager_port_conf(pci_address)[key];
	} catch (YAML::Exception& e) {
		XDPD_ERR(DRIVER_NAME" dpdk port: %s, setting: \"%s\" not found, aborting\n", pci_address.c_str(), key.c_str());
		throw;
	}
}

/**
*
*/
template<typename T> T iface_manager_get_port_setting_as(const std::string& pci_address, const std::string& key){
	try {
		return iface_manager_port_conf(pci_address)[key].as<T>();
	} catch (YAML::Exception& e) {
		XDPD_ERR(DRIVER_NAME" dpdk port: %s, setting: \"%s\" not found, aborting\n", pci_address.c_str(), key.c_str());
		throw;
	}
}


static uint16_t iface_manager_pci_address_to_port_id(const std::string& pci_addr){
	struct rte_eth_dev_info dev_info;
	char s_pci_addr[64];

	//Calculate size of rte_mempool for rxqueue/txqueue configuration based on available physical ports
	for (uint16_t port_id = 0; port_id < rte_eth_dev_count(); port_id++) {
		rte_eth_dev_info_get(port_id, &dev_info);
		if ((port_id >= RTE_MAX_ETHPORTS) || (!dev_info.pci_dev)) {
			throw std::exception();
		}
		memset(s_pci_addr, 0, sizeof(s_pci_addr));
		rte_pci_device_name(&(dev_info.pci_dev->addr), s_pci_addr, sizeof(s_pci_addr));

		if (!pci_addr.compare(s_pci_addr)) {
			return port_id;
		}
	}
	throw std::exception();
}

/**
* Setup virtual ports.
*/
rofl_result_t iface_manager_setup_virtual_ports(void){

	int ret = 0;
	unsigned int port_name_index = 0;

	YAML::Node knis_node = y_config_dpdk_ng["dpdk"]["knis"];
	if (knis_node && knis_node.IsMap()) {

		for (auto it : knis_node) {
			YAML::Node& kni_name_node = it.first;
			YAML::Node  kni_args_node = it.second["args"];
			YAML::Node  kni_enabled_node = it.second["enabled"];

			if (not kni_name_node || not kni_name_node.IsScalar()) {
				continue;
			}

			if (kni_enabled_node && kni_enabled_node.IsScalar() && (kni_enabled_node.as<bool>() == false)) {
				continue;
			}

			strncpy(vport_names[port_name_index], kni_name_node.as<std::string>().c_str(), SWITCH_PORT_MAX_LEN_NAME);
			std::string ifname(vport_names[port_name_index]);

			/* assumption: ifname = "kni0", "kni1", ..., TODO: add check for "kniN" */
			std::string knidev_name = std::string("net_kni_") + ifname;

			std::string knidev_args;
			if (kni_args_node && kni_args_node.IsScalar()) {
				knidev_args = kni_args_node.as<std::string>();
			}

			XDPD_INFO(DRIVER_NAME"[ifaces] adding virtual PMD kni port: %s with args: %s\n", knidev_name.c_str(), knidev_args.c_str());

			/* initialize kni pmd device */
			if ((ret = rte_vdev_init(knidev_name.c_str(), knidev_args.c_str())) < 0) {
				switch (ret) {
				case -EINVAL: {
					XDPD_ERR(DRIVER_NAME"[ifaces] initialization of kni dev %s with args \"%s\" failed (EINVAL)\n",
							ifname.c_str(), knidev_args.c_str());
				} break;
				case -EEXIST: {
					XDPD_ERR(DRIVER_NAME"[ifaces] initialization of kni dev %s with args \"%s\" failed (EEXIST)\n",
							ifname.c_str(), knidev_args.c_str());
				} break;
				case -ENOMEM: {
					XDPD_ERR(DRIVER_NAME"[ifaces] initialization of kni dev %s with args \"%s\" failed (ENOMEM)\n",
							ifname.c_str(), knidev_args.c_str());
				} break;
				default: {
					XDPD_ERR(DRIVER_NAME"[ifaces] initialization of kni dev %s with args \"%s\" failed\n",
							ifname.c_str(), knidev_args.c_str());
				};
				}
				return ROFL_FAILURE;
			}

			port_name_index++;
		}
	}

	YAML::Node rings_node = y_config_dpdk_ng["dpdk"]["rings"];
	if (rings_node && rings_node.IsMap()) {

		for (auto it : rings_node) {
			YAML::Node& ring_name_node = it.first;
			YAML::Node  ring_peer_node = it.second["ring_peer"];
			YAML::Node  ring_size_node = it.second["ring_size"];
			YAML::Node  socket_id_node = it.second["socket_id"];
			YAML::Node  ring_enabled_node = it.second["enabled"];

			if (not ring_name_node || not ring_name_node.IsScalar()) {
				continue;
			}
			std::string ifname(ring_name_node.as<std::string>());

			if (ring_enabled_node && ring_enabled_node.IsScalar() && (ring_enabled_node.as<bool>() == false)) {
				continue;
			}

			std::string peername = ifname + "p";
			if (ring_peer_node && ring_peer_node.IsScalar()) {
				peername = ring_peer_node.as<std::string>();
			}
			strncpy(vport_names[port_name_index++], ifname.c_str(), SWITCH_PORT_MAX_LEN_NAME);
			strncpy(vport_names[port_name_index++], peername.c_str(), SWITCH_PORT_MAX_LEN_NAME);

			unsigned int ring_size = 256; //default ring size
			if (ring_size_node && ring_size_node.IsScalar()) {
				ring_size = ring_size_node.as<unsigned int>();
			}

			unsigned int socket_id = 0; //default socket id
			if (socket_id_node && socket_id_node.IsScalar()) {
				socket_id = socket_id_node.as<unsigned int>();
			}

			std::string ring_name_0 = std::string("ring_") + ifname;
			std::string ring_name_1 = std::string("ring_") + peername;

			/* TODO: Do we have to keep pointers to these ring backends created for the two ports
			 * for later deallocation? Or are those simply free'd once the ethernet devices
			 * are removed? */
			struct rte_ring *ring[2];
			ring[0] = rte_ring_create(ring_name_0.c_str(), ring_size, socket_id, RING_F_SP_ENQ|RING_F_SC_DEQ);
			ring[1] = rte_ring_create(ring_name_1.c_str(), ring_size, socket_id, RING_F_SP_ENQ|RING_F_SC_DEQ);

			if((ret=rte_eth_from_rings(ifname.c_str(), &ring[0], 1, &ring[1], 1, socket_id)) < 0){
				switch (rte_errno){
				case EINVAL:{
					XDPD_INFO(DRIVER_NAME"[ifaces] adding virtual PMD ring port: %s failed (EINVAL)\n", ifname.c_str());
				}break;
				default:{
					XDPD_INFO(DRIVER_NAME"[ifaces] adding virtual PMD ring port: %s failed\n", ifname.c_str());
				}break;
				}
				return ROFL_FAILURE;
			}

			if((ret=rte_eth_from_rings(peername.c_str(), &ring[1], 1, &ring[0], 1, socket_id)) < 0){
				switch (rte_errno){
				case EINVAL:{
					XDPD_INFO(DRIVER_NAME"[ifaces] adding virtual PMD ring port: %s failed (EINVAL)\n", ifname.c_str());
				}break;
				default:{
					XDPD_INFO(DRIVER_NAME"[ifaces] adding virtual PMD ring port: %s failed\n", ifname.c_str());
				}break;
				}
				return ROFL_FAILURE;
			}

			XDPD_INFO(DRIVER_NAME"[ifaces] adding virtual PMD ring port %s with peer %s\n", ifname.c_str(), peername.c_str());
		}
	}

	YAML::Node pcaps_node = y_config_dpdk_ng["dpdk"]["pcaps"];
	if (pcaps_node && pcaps_node.IsMap()) {

		for (auto it : pcaps_node) {
			YAML::Node& pcap_name_node = it.first;
			YAML::Node  pcap_args_node = it.second["args"];
			YAML::Node  pcap_enabled_node = it.second["enabled"];

			if (not pcap_name_node || not pcap_name_node.IsScalar()) {
				continue;
			}

			if (pcap_enabled_node && pcap_enabled_node.IsScalar() && (pcap_enabled_node.as<bool>() == false)) {
				continue;
			}

			strncpy(vport_names[port_name_index], pcap_name_node.as<std::string>().c_str(), SWITCH_PORT_MAX_LEN_NAME);
			std::string ifname(vport_names[port_name_index]);

			/* assumption: ifname = "pcap0", "pcap1", ..., TODO: add check for "pcapN" */
			std::string pcapdev_name = std::string("net_pcap_") + ifname;

			std::string pcapdev_args;
			if (pcap_args_node && pcap_args_node.IsScalar()) {
				pcapdev_args = pcap_args_node.as<std::string>();
			}

			XDPD_INFO(DRIVER_NAME"[ifaces] adding virtual PMD pcap port: %s with args: %s\n", pcapdev_name.c_str(), pcapdev_args.c_str());

			/* initialize pcap pmd device */
			if ((ret = rte_vdev_init(pcapdev_name.c_str(), pcapdev_args.c_str())) < 0) {
				switch (ret) {
				case -EINVAL: {
					XDPD_ERR(DRIVER_NAME"[ifaces] initialization of pcap dev %s with args \"%s\" failed (EINVAL)\n",
							ifname.c_str(), pcapdev_args.c_str());
				} break;
				case -EEXIST: {
					XDPD_ERR(DRIVER_NAME"[ifaces] initialization of pcap dev %s with args \"%s\" failed (EEXIST)\n",
							ifname.c_str(), pcapdev_args.c_str());
				} break;
				case -ENOMEM: {
					XDPD_ERR(DRIVER_NAME"[ifaces] initialization of pcap dev %s with args \"%s\" failed (ENOMEM)\n",
							ifname.c_str(), pcapdev_args.c_str());
				} break;
				default: {
					XDPD_ERR(DRIVER_NAME"[ifaces] initialization of pcap dev %s with args \"%s\" failed\n",
							ifname.c_str(), pcapdev_args.c_str());
				};
				}
				return ROFL_FAILURE;
			}

			port_name_index++;
		}
	}

	return ROFL_SUCCESS;
}

/**
* Discovers physical ports.
*/
rofl_result_t iface_manager_discover_physical_ports(void){

	struct rte_eth_dev_info dev_info;
	char s_fw_version[256];
	char s_pci_addr[64];
	std::map<unsigned int, size_t> nb_mbuf; //The required number of mbufs per NUMA node => <socket_id, nb_mbuf>
	YAML::Node node;
	int ret = 0;

	//Initialize physical port structure: all phyports disabled
	for (uint16_t port_id = 0; port_id < rte_eth_dev_count(); port_id++) {
		phyports[port_id].socket_id = -1;
		phyports[port_id].is_enabled = 0;
		phyports[port_id].nb_rx_queues = 0;
		phyports[port_id].nb_tx_queues = 0;
		phyports[port_id].is_vf = 0;
		phyports[port_id].parent_port_id = -1;
		phyports[port_id].nb_vfs = 0;
		phyports[port_id].vf_id = -1;
		phyports[port_id].is_virtual = 0;
	}

	//Calculate size of rte_mempool for rxqueue/txqueue configuration based on available physical ports
	for (uint16_t port_id = 0; port_id < rte_eth_dev_count(); port_id++) {

		char ifname[32];
		char portname[IF_NAMESIZE];
		if ((ret = rte_eth_dev_get_name_by_port(port_id, ifname)) < 0) {
			continue;
		}

		rte_eth_dev_info_get(port_id, &dev_info);
		if (dev_info.pci_dev) {
			memset(s_pci_addr, 0, sizeof(s_pci_addr));
			rte_pci_device_name(&(dev_info.pci_dev->addr), s_pci_addr, sizeof(s_pci_addr));
		}

		if (port_id >= RTE_MAX_ETHPORTS) {
			return ROFL_FAILURE;
		}

		// port not specified in configuration file
		if (not iface_manager_port_exists(s_pci_addr)) {
			continue;
		}

		// port disabled in configuration file?
		if (not iface_manager_get_port_setting_as<bool>(s_pci_addr, "enabled")) {
			continue;
		}

		int socket_id = rte_eth_dev_socket_id(port_id);

		/* virtual ports appear as physical ones here (including kni, ring, ...)
		 * However, they are bound to NUMA node SOCKET_ID_ANY. We bind all those
		 * virtual devices to the NUMA socket specified in the configuration file. */

		/* for ports bound to SOCKET_ID_ANY (virtual interfaces, e.g., kni), use socket_id as specified in configuration file or master lcore as default */
		if (dev_info.driver_name == std::string("net_kni")) {
			snprintf (portname, SWITCH_PORT_MAX_LEN_NAME, ifname+8); //strip off "net_kni_"
			YAML::Node kni_node = y_config_dpdk_ng["dpdk"]["knis"][portname]["socket_id"];
			if (kni_node && kni_node.IsScalar()) {
				socket_id = kni_node.as<int>();
				if (numa_nodes.find(socket_id) == numa_nodes.end()) {
					XDPD_ERR(DRIVER_NAME"[ifaces] virtual port: %u, invalid socket %u specified, ignoring\n", port_id, socket_id);
					continue;
				}
			} else {
				socket_id = rte_lcore_to_socket_id(rte_get_master_lcore());
			}
			XDPD_INFO(DRIVER_NAME"[ifaces] virtual port: %u, mapping SOCKET_ID_ANY to socket %u\n", port_id, socket_id);
			phyports[port_id].is_virtual = true;
		} else
		if (dev_info.driver_name == std::string("net_pcap")) {
			snprintf (portname, SWITCH_PORT_MAX_LEN_NAME, ifname+9); //strip off "net_pcap_"
			YAML::Node pcap_node = y_config_dpdk_ng["dpdk"]["pcaps"][portname]["socket_id"];
			if (pcap_node && pcap_node.IsScalar()) {
				socket_id = pcap_node.as<int>();
				if (numa_nodes.find(socket_id) == numa_nodes.end()) {
					XDPD_ERR(DRIVER_NAME"[ifaces] virtual port: %u, invalid socket %u specified, ignoring\n", port_id, socket_id);
					continue;
				}
			} else {
				socket_id = rte_lcore_to_socket_id(rte_get_master_lcore());
			}
			XDPD_INFO(DRIVER_NAME"[ifaces] virtual port: %u, mapping SOCKET_ID_ANY to socket %u\n", port_id, socket_id);
			phyports[port_id].is_virtual = true;
		} else
		if (dev_info.driver_name == std::string("net_ring")) {
			snprintf (portname, SWITCH_PORT_MAX_LEN_NAME, ifname+9); //strip off "net_ring_"
			YAML::Node ring_node = y_config_dpdk_ng["dpdk"]["rings"][portname]["socket_id"];
			if (ring_node && ring_node.IsScalar()) {
				socket_id = ring_node.as<int>();
				if (numa_nodes.find(socket_id) == numa_nodes.end()) {
					XDPD_ERR(DRIVER_NAME"[ifaces] virtual port: %u, invalid socket %u specified, ignoring\n", port_id, socket_id);
					continue;
				}
			} else {
				socket_id = rte_lcore_to_socket_id(rte_get_master_lcore());
			}
			XDPD_INFO(DRIVER_NAME"[ifaces] virtual port: %u, mapping SOCKET_ID_ANY to socket %u\n", port_id, socket_id);
			phyports[port_id].is_virtual = true;
		}

		phyports[port_id].socket_id = socket_id;

		unsigned int nb_rx_queues = RTE_MIN(rx_lcores[socket_id].size(), dev_info.max_rx_queues);
		unsigned int nb_tx_queues = RTE_MIN(tx_lcores[socket_id].size(), dev_info.max_tx_queues);

		// initialize nb_mbuf for socket_id
		if (nb_mbuf.find(socket_id)==nb_mbuf.end()){
			nb_mbuf[socket_id] = 0;
		}

		//nb_mbuf[socket_id] += /*rx*/nb_rx_queues * dev_info.rx_desc_lim.nb_max + /*tx*/nb_tx_queues * dev_info.tx_desc_lim.nb_max;
		nb_mbuf[socket_id] += /*rx*/dev_info.rx_desc_lim.nb_max + /*tx*/dev_info.tx_desc_lim.nb_max;
		XDPD_INFO(DRIVER_NAME"[ifaces] calculating memory needs for port %u, nb_rx_queues=%u, nb_tx_queues=%u, rx_desc_lim.nb_max=%u, tx_desc_lim.nb_max=%u => nb_mbuf[socket_id=%u]=%u\n",
				port_id, nb_rx_queues, nb_tx_queues, dev_info.rx_desc_lim.nb_max, dev_info.tx_desc_lim.nb_max, socket_id, nb_mbuf[socket_id]);
	}


	//Allocate mempools on all NUMA sockets
	for (auto socket_id : numa_nodes) {
		unsigned int nbmbufs = (unsigned int)pow(2, ceil(log2((mem_pool_size == 0) ? nb_mbuf[socket_id] : mem_pool_size)));
		unsigned int pool_size = RTE_MIN(nbmbufs, (uint32_t)UINT32_C(1<<31));
		XDPD_INFO(DRIVER_NAME"[ifaces] allocating memory, pool_size: %u, data_room: %u\n", pool_size, mbuf_dataroom);
		memory_init(socket_id, pool_size, mbuf_dataroom,
				dpdk_memory_mempool_direct_cache_size,
				dpdk_memory_mempool_direct_priv_size,
				dpdk_memory_mempool_indirect_cache_size,
				dpdk_memory_mempool_indirect_priv_size);
	}


	//Iterate over all available physical ports
	for (uint16_t port_id = 0; port_id < rte_eth_dev_count(); port_id++) {

		char ifname[IF_NAMESIZE];
		if ((ret = rte_eth_dev_get_name_by_port(port_id, ifname)) < 0) {
			continue;
		}

		rte_eth_dev_info_get(port_id, &dev_info);
		memset(s_pci_addr, 0, sizeof(s_pci_addr));
		if (dev_info.pci_dev) {
			rte_pci_device_name(&(dev_info.pci_dev->addr), s_pci_addr, sizeof(s_pci_addr));
		}

		if (port_id >= RTE_MAX_ETHPORTS) {
			return ROFL_FAILURE;
		}

		phyports[port_id].is_enabled = 0;
		unsigned int socket_id = phyports[port_id].socket_id;

		rte_eth_dev_info_get(port_id, &dev_info);
		strncpy(s_fw_version, "none", sizeof(s_fw_version)-1);
		rte_eth_dev_fw_version_get(port_id, s_fw_version, sizeof(s_fw_version));

		if ((ret = rte_eth_dev_reset(port_id)) < 0) {
			XDPD_INFO(DRIVER_NAME"[ifaces] warning on physical port: %u (device reset failed) on socket: %u, driver: %s, firmware: %s, PCI address: %s\n",
					port_id, socket_id, dev_info.driver_name, s_fw_version, s_pci_addr);
			//continue;
		}

		// port not specified in configuration file
		if (not phyports[port_id].is_virtual && not iface_manager_port_exists(s_pci_addr)) {
			XDPD_INFO(DRIVER_NAME"[ifaces] skipping physical port: %u (not found in configuration, assuming state \"disabled\") on socket: %u, driver: %s, firmware: %s, PCI address: %s\n",
					port_id, socket_id, dev_info.driver_name, s_fw_version, s_pci_addr);
			continue;
		}

		//get devname for specified port
		const std::string devname(iface_manager_get_port_setting_as<std::string>(s_pci_addr, "ifname"));

		// port disabled in configuration file?
		if (not phyports[port_id].is_virtual && not iface_manager_get_port_setting_as<bool>(s_pci_addr, "enabled")) {
			XDPD_INFO(DRIVER_NAME"[ifaces][%s] skipping physical port: %u (port explicitly \"disabled\") on socket: %u, driver: %s, firmware: %s, PCI address: %s\n",
					devname.c_str(), port_id, socket_id, dev_info.driver_name, s_fw_version, s_pci_addr);
			continue;
		}

		phyports[port_id].is_enabled = 1;

		// is port a virtual function and has a parent device?
		if (not phyports[port_id].is_virtual && iface_manager_port_setting_exists(s_pci_addr, "parent")) {
			phyports[port_id].is_vf = 1;
			if (iface_manager_port_exists(iface_manager_get_port_setting_as<std::string>(s_pci_addr, "parent"))) {
				phyports[port_id].parent_port_id = iface_manager_pci_address_to_port_id(iface_manager_get_port_setting_as<std::string>(s_pci_addr, "parent"));
			}
			phyports[port_id].vf_id = phyports[phyports[port_id].parent_port_id].nb_vfs++;
			if (phyports[port_id].parent_port_id == port_id) {
				XDPD_ERR(DRIVER_NAME"[ifaces][%s] unlikely configuration detected: parent port_id == port_id (%u), probably a misconfiguration?\n", devname.c_str(), port_id);
			}
		}

		//number of configured RX queues on device should not exceed number of worker lcores on socket
		unsigned int nb_rx_queues = RTE_MIN(rx_lcores[socket_id].size(), dev_info.max_rx_queues);

		//number of configured TX queues on device should not exceed number of worker lcores on socket
		unsigned int nb_tx_queues = RTE_MIN(tx_lcores[socket_id].size(), dev_info.max_tx_queues);

		phyports[port_id].nb_rx_queues = nb_rx_queues;
		phyports[port_id].nb_tx_queues = nb_tx_queues;

		if (phyports[port_id].nb_rx_queues == 0) {
			XDPD_INFO(DRIVER_NAME"[ifaces][%s] skipping physical port: %u on socket: %u with nb_rx_queues: %u\n",
					devname.c_str(), port_id, socket_id, nb_rx_queues);
			continue;
		}

		if (phyports[port_id].nb_tx_queues == 0) {
			XDPD_INFO(DRIVER_NAME"[ifaces][%s] skipping physical port: %u on socket: %u with nb_tx_queues: %u\n",
					devname.c_str(), port_id, socket_id, nb_tx_queues);
			continue;
		}


		XDPD_INFO(DRIVER_NAME"[ifaces][%s] adding physical port: %u on socket: %u with max_rx_queues: %u, rx_queues in use: %u, max_tx_queues: %u, tx_queues in use: %u, driver: %s, firmware: %s, PCI address: %s\n",
				devname.c_str(), port_id, socket_id, dev_info.max_rx_queues, nb_rx_queues, dev_info.max_tx_queues, nb_tx_queues, dev_info.driver_name, s_fw_version, s_pci_addr);


		/* all RX lcores for this port's (port_id) NUMA node (socket_id) */
		uint16_t rx_queue_id = 0;
		for (auto lcore_id : rx_lcores[socket_id]) {
			if (not lcores[lcore_id].is_enabled) {
				continue;
			}
			if (not lcores[lcore_id].is_rx_lcore) {
				continue;
			}

			uint16_t index = rx_core_tasks[lcore_id].nb_rx_queues;
			rx_core_tasks[lcore_id].rx_queues[index].up = false;
			rx_core_tasks[lcore_id].rx_queues[index].port_id = port_id;
			rx_core_tasks[lcore_id].rx_queues[index].queue_id = rx_queue_id;
			rx_core_tasks[lcore_id].nb_rx_queues++;
			XDPD_INFO(DRIVER_NAME"[ifaces][%s] assigning physical port: %u, rxqueue: %u on socket: %u to lcore: %u on socket: %u, nb_rx_queues: %u\n",
					devname.c_str(), port_id, rx_queue_id, socket_id, lcore_id, rte_lcore_to_socket_id(lcore_id), rx_core_tasks[lcore_id].nb_rx_queues);
			if (rx_queue_id >= (phyports[port_id].nb_rx_queues - 1)) {
				break;
			}
			rx_queue_id = (rx_queue_id < (phyports[port_id].nb_rx_queues - 1)) ? rx_queue_id + 1 : 0;
		}


		/* all TX lcores for this port's (port_id) NUMA node (socket_id) */
		uint16_t tx_queue_id = 0;
		for (auto lcore_id : tx_lcores[socket_id]) {
			if (not lcores[lcore_id].is_enabled) {
				continue;
			}
			if (not lcores[lcore_id].is_tx_lcore) {
				continue;
			}


			/*
			 * RTE tx ring for this port
			 */

			/* allocate txring for tx-task */
			std::stringstream rgname("tx-ring-");
			rgname << "task-" << lcore_id << "." << "port-" << port_id;

			/* store txring-drain-max-queuesize parameter for this port */
			if (not phyports[port_id].is_virtual && iface_manager_port_setting_exists(s_pci_addr, "txring_drain_queue_capacity")) {
				tx_core_tasks[lcore_id].txring_drain_queue_capacity[port_id] = pow(2, (unsigned int)ceil(log2(iface_manager_get_port_setting_as<unsigned int>(s_pci_addr, "txring_drain_queue_capacity"))));
			} else {
				tx_core_tasks[lcore_id].txring_drain_queue_capacity[port_id] = pow(2, (unsigned int)ceil(log2((unsigned int)PROCESSING_TXRING_DRAIN_QUEUE_CAPACITY_DEFAULT)));
			}

			/* store txring-drain-interval parameter for this port */
			uint64_t txring_drain_interval;
			if (not phyports[port_id].is_virtual && iface_manager_port_setting_exists(s_pci_addr, "txring_drain_interval")) {
				txring_drain_interval = iface_manager_get_port_setting_as<uint64_t>(s_pci_addr, "txring_drain_interval") * /*number of cycles in 1us for default timer=*/(rte_get_timer_hz() / 1e6);
			} else {
				txring_drain_interval = PROCESSING_TXRING_DRAIN_INTERVAL_DEFAULT * /*number of cycles in 1us for default timer=*/(rte_get_timer_hz() / 1e6);
			}
			tx_core_tasks[lcore_id].txring_drain_interval[port_id] = txring_drain_interval;

			/* store txring-drain-threshold parameter for this port */
			if (not phyports[port_id].is_virtual && iface_manager_port_setting_exists(s_pci_addr, "txring_drain_threshold")) {
				tx_core_tasks[lcore_id].txring_drain_threshold[port_id] = iface_manager_get_port_setting_as<unsigned int>(s_pci_addr, "txring_drain_threshold");
			} else {
				tx_core_tasks[lcore_id].txring_drain_threshold[port_id] = PROCESSING_TXRING_DRAIN_THRESHOLD_DEFAULT;
			}

			XDPD_INFO(DRIVER_NAME"[ifaces][%s] physical port: %u on socket: %u for tx-task-%02u, txring: %s, capacity: %u\n",
					devname.c_str(), port_id, socket_id, lcore_id, rgname.str().c_str(), tx_core_tasks[lcore_id].txring_drain_queue_capacity[port_id]);

			/* create RTE ring for queuing packets between workers and tx threads */
			if ((tx_core_tasks[lcore_id].txring[port_id] = rte_ring_create(rgname.str().c_str(), tx_core_tasks[lcore_id].txring_drain_queue_capacity[port_id], socket_id, RING_F_SP_ENQ | RING_F_SC_DEQ)) == NULL) {
				XDPD_DEBUG(DRIVER_NAME"[ifaces] unable to create tx-ring: %s for port-id: %u\n", rgname.str().c_str(), port_id);
				return ROFL_FAILURE;
			}


			/*
			 * store txqueue on eth-dev for this port and TX task
			 */

			tx_core_tasks[lcore_id].tx_queues[port_id].enabled = 1;
			tx_core_tasks[lcore_id].tx_queues[port_id].up = false;
			tx_core_tasks[lcore_id].tx_queues[port_id].queue_id = tx_queue_id;
			tx_core_tasks[lcore_id].nb_tx_queues++;
			XDPD_INFO(DRIVER_NAME"[ifaces][%s] assigning physical port: %u, txqueue: %u on socket: %u to lcore: %u on socket: %u, nb_tx_queues: %u\n",
					devname.c_str(), port_id, tx_queue_id, socket_id, lcore_id, rte_lcore_to_socket_id(lcore_id), tx_core_tasks[lcore_id].nb_tx_queues);
			/* if the number of TX lcores exceeds the number of tx-queues on the port and
			 * the TX queue is not able to handle multiple threads without locking, 
			 * abort here and give a hint to the user */
			if ((not (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MT_LOCKFREE)) && (tx_queue_id > (phyports[port_id].nb_tx_queues - 1))) {
				XDPD_ERR(DRIVER_NAME"[ifaces][%s] number of TX tasks on NUMA node socket %u exceeds number of TX queues on port %u (%s) and port is not DEV_TX_OFFLOAD_MT_LOCKFREE capable, you have 3 options:\n", devname.c_str(), socket_id, port_id, ifname);
				XDPD_ERR(DRIVER_NAME"[ifaces][%s] 1. increase number of TX queues on port %s\n", devname.c_str(), ifname);
				XDPD_ERR(DRIVER_NAME"[ifaces][%s] 2. reduce number of TX tasks on NUMA node socket %u\n", devname.c_str(), socket_id);
				XDPD_ERR(DRIVER_NAME"[ifaces][%s] 3. disable port %s\n", devname.c_str(), ifname);
				XDPD_ERR(DRIVER_NAME"[ifaces][%s] aborting ...\n", devname.c_str());
				return ROFL_FAILURE;
			}
			tx_queue_id = (tx_queue_id < (phyports[port_id].nb_tx_queues - 1)) ? tx_queue_id + 1 : 0;
		}



		//Configure the port
		struct rte_eth_conf eth_conf;
		memset(&eth_conf, 0, sizeof(eth_conf));


		uint32_t max_rx_pkt_len(MAX_RX_PKT_LEN_DEFAULT);
		YAML::Node offload_max_rx_pkt_len_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["max_rx_pkt_len"];
		if (offload_max_rx_pkt_len_node && offload_max_rx_pkt_len_node.IsScalar()) {
			max_rx_pkt_len = offload_max_rx_pkt_len_node.as<uint32_t>();
		}

		//activate all rx offload capabilities by default
		uint64_t rx_offloads = dev_info.rx_offload_capa;


		/* workaround for i40evf PMD driver with kernel based i40e PF
		 * dev_info.rx_offload_capa does not contain DEV_RX_OFFLOAD_CRC_STRIP, although
		 * it is mandatory by the kernel based i40e driver, so we add
		 * DEV_RX_OFFLOAD_CRC_STRIP to the set of available rx_offload capas
		 */
		if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_I40E_VF, sizeof(DPDK_DRIVER_NAME_I40E_VF)) == 0){
			XDPD_INFO(DRIVER_NAME"[ifaces][%s] adding DEV_RX_OFFLOAD_CRC_STRIP workaround to rx_offload capabilities for %s driver\n", devname.c_str(), DPDK_DRIVER_NAME_I40E_VF);
			rx_offloads |= DEV_RX_OFFLOAD_CRC_STRIP;
		}

		/*
		 * deactivate certain offload features based on user configuration
		 */

		//VLAN STRIP
		YAML::Node offload_rx_vlan_strip_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["rx_vlan_strip"];
		if (offload_rx_vlan_strip_node && offload_rx_vlan_strip_node.IsScalar() && (not offload_rx_vlan_strip_node.as<bool>())) {
			rx_offloads &= ~DEV_RX_OFFLOAD_VLAN_STRIP;
		}
		//IPV4 CKSUM
		YAML::Node offload_rx_ipv4_cksum_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["rx_ipv4_cksum"];
		if (offload_rx_ipv4_cksum_node && offload_rx_ipv4_cksum_node.IsScalar() && (not offload_rx_ipv4_cksum_node.as<bool>())) {
			rx_offloads &= ~DEV_RX_OFFLOAD_IPV4_CKSUM;
		}
		//UDP CKSUM
		YAML::Node offload_rx_udp_cksum_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["rx_udp_cksum"];
		if (offload_rx_udp_cksum_node && offload_rx_udp_cksum_node.IsScalar() && (not offload_rx_udp_cksum_node.as<bool>())) {
			rx_offloads &= ~DEV_RX_OFFLOAD_UDP_CKSUM;
		}
		//TCP CKSUM
		YAML::Node offload_rx_tcp_cksum_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["rx_tcp_cksum"];
		if (offload_rx_tcp_cksum_node && offload_rx_tcp_cksum_node.IsScalar() && (not offload_rx_tcp_cksum_node.as<bool>())) {
			rx_offloads &= ~DEV_RX_OFFLOAD_TCP_CKSUM;
		}
		//TCP LRO
		YAML::Node offload_rx_tcp_lro_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["rx_tcp_lro"];
		if (offload_rx_tcp_lro_node && offload_rx_tcp_lro_node.IsScalar() && (not offload_rx_tcp_lro_node.as<bool>())) {
			rx_offloads &= ~DEV_RX_OFFLOAD_TCP_LRO;
		}
		//QINQ STRIP
		YAML::Node offload_rx_qinq_strip_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["rx_qinq_strip"];
		if (offload_rx_qinq_strip_node && offload_rx_qinq_strip_node.IsScalar() && (not offload_rx_qinq_strip_node.as<bool>())) {
			rx_offloads &= ~DEV_RX_OFFLOAD_QINQ_STRIP;
		}
		//OUTER IPV4 CKSUM
		YAML::Node offload_rx_outer_ipv4_cksum_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["rx_outer_ipv4_cksum"];
		if (offload_rx_outer_ipv4_cksum_node && offload_rx_outer_ipv4_cksum_node.IsScalar() && (not offload_rx_outer_ipv4_cksum_node.as<bool>())) {
			rx_offloads &= ~DEV_RX_OFFLOAD_OUTER_IPV4_CKSUM;
		}
		//MACSEC STRIP
		YAML::Node offload_rx_macsec_strip_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["rx_macsec_strip"];
		if (offload_rx_macsec_strip_node && offload_rx_macsec_strip_node.IsScalar() && (not offload_rx_macsec_strip_node.as<bool>())) {
			rx_offloads &= ~DEV_RX_OFFLOAD_MACSEC_STRIP;
		}
		//HEADER SPLIT
		YAML::Node offload_rx_header_split_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["rx_header_split"];
		if (offload_rx_header_split_node && offload_rx_header_split_node.IsScalar() && (not offload_rx_header_split_node.as<bool>())) {
			rx_offloads &= ~DEV_RX_OFFLOAD_HEADER_SPLIT;
		}
		//VLAN FILTER
		YAML::Node offload_rx_vlan_filter_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["rx_vlan_filter"];
		if (offload_rx_vlan_filter_node && offload_rx_vlan_filter_node.IsScalar() && (not offload_rx_vlan_filter_node.as<bool>())) {
			rx_offloads &= ~DEV_RX_OFFLOAD_VLAN_FILTER;
		}
		//VLAN EXTEND
		YAML::Node offload_rx_vlan_extend_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["rx_vlan_extend"];
		if (offload_rx_vlan_extend_node && offload_rx_vlan_extend_node.IsScalar() && (not offload_rx_vlan_extend_node.as<bool>())) {
			rx_offloads &= ~DEV_RX_OFFLOAD_VLAN_EXTEND;
		}
		//JUMBO FRAME
		YAML::Node offload_rx_jumbo_frame_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["rx_jumbo_frame"];
		if (offload_rx_jumbo_frame_node && offload_rx_jumbo_frame_node.IsScalar() && (not offload_rx_jumbo_frame_node.as<bool>())) {
			rx_offloads &= ~DEV_RX_OFFLOAD_JUMBO_FRAME;
		}
		//CRC STRIP
		YAML::Node offload_rx_crc_strip_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["rx_crc_strip"];
		if (offload_rx_crc_strip_node && offload_rx_crc_strip_node.IsScalar() && (not offload_rx_crc_strip_node.as<bool>())) {
			rx_offloads &= ~DEV_RX_OFFLOAD_CRC_STRIP;
		}
		//SCATTER
		YAML::Node offload_rx_scatter_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["rx_scatter"];
		if (offload_rx_scatter_node && offload_rx_scatter_node.IsScalar() && (not offload_rx_scatter_node.as<bool>())) {
			rx_offloads &= ~DEV_RX_OFFLOAD_SCATTER;
		}
		//TIMESTAMP
		YAML::Node offload_rx_timestamp_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["rx_timestamp"];
		if (offload_rx_timestamp_node && offload_rx_timestamp_node.IsScalar() && (not offload_rx_timestamp_node.as<bool>())) {
			rx_offloads &= ~DEV_RX_OFFLOAD_TIMESTAMP;
		}
		//SECURITY
		YAML::Node offload_rx_security_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["rx_security"];
		if (offload_rx_security_node && offload_rx_security_node.IsScalar() && (not offload_rx_security_node.as<bool>())) {
			rx_offloads &= ~DEV_RX_OFFLOAD_SECURITY;
		}

		//helper string
		std::stringstream s_rx_offloads;
		if (rx_offloads & DEV_RX_OFFLOAD_VLAN_STRIP){
			s_rx_offloads << "RX_VLAN_STRIP, ";
		}
		if (rx_offloads & DEV_RX_OFFLOAD_IPV4_CKSUM){
			s_rx_offloads << "RX_IPV4_CKSUM, ";
		}
		if (rx_offloads & DEV_RX_OFFLOAD_UDP_CKSUM){
			s_rx_offloads << "RX_UDP_CKSUM, ";
		}
		if (rx_offloads & DEV_RX_OFFLOAD_TCP_CKSUM){
			s_rx_offloads << "RX_TCP_CKSUM, ";
		}
		if (rx_offloads & DEV_RX_OFFLOAD_TCP_LRO){
			s_rx_offloads << "RX_TCP_LRO, ";
		}
		if (rx_offloads & DEV_RX_OFFLOAD_QINQ_STRIP){
			s_rx_offloads << "RX_QINQ_STRIP, ";
		}
		if (rx_offloads & DEV_RX_OFFLOAD_OUTER_IPV4_CKSUM){
			s_rx_offloads << "RX_OUTER_IPV4_CKSUM, ";
		}
		if (rx_offloads & DEV_RX_OFFLOAD_MACSEC_STRIP){
			s_rx_offloads << "RX_MACSEC_STRIP, ";
		}
		if (rx_offloads & DEV_RX_OFFLOAD_HEADER_SPLIT){
			s_rx_offloads << "RX_HEADER_SPLIT, ";
		}
		if (rx_offloads & DEV_RX_OFFLOAD_VLAN_FILTER){
			s_rx_offloads << "RX_VLAN_FILTER, ";
		}
		if (rx_offloads & DEV_RX_OFFLOAD_VLAN_EXTEND){
			s_rx_offloads << "RX_VLAN_EXTEND, ";
		}
		if (rx_offloads & DEV_RX_OFFLOAD_JUMBO_FRAME){
			s_rx_offloads << "RX_JUMBO_FRAME, ";
		}
		if (rx_offloads & DEV_RX_OFFLOAD_CRC_STRIP){
			s_rx_offloads << "RX_CRC_STRIP, ";
		}
		if (rx_offloads & DEV_RX_OFFLOAD_SCATTER){
			s_rx_offloads << "RX_SCATTER, ";
		}
		if (rx_offloads & DEV_RX_OFFLOAD_TIMESTAMP){
			s_rx_offloads << "RX_TIMESTAMP, ";
		}
		if (rx_offloads & DEV_RX_OFFLOAD_SECURITY){
			s_rx_offloads << "RX_SECURITY, ";
		}

		//activate all tx offload capabilities by default
		uint64_t tx_offloads = dev_info.tx_offload_capa;

		/*
		 * deactivate certain offload features based on user configuration
		 */

		//VLAN INSERT
		YAML::Node offload_tx_vlan_insert_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["tx_vlan_insert"];
		if (offload_tx_vlan_insert_node && offload_tx_vlan_insert_node.IsScalar() && (not offload_tx_vlan_insert_node.as<bool>())) {
			tx_offloads &= ~DEV_TX_OFFLOAD_VLAN_INSERT;
		}
		//IPV4 CKSUM
		YAML::Node offload_tx_ipv4_cksum_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["tx_ipv4_cksum"];
		if (offload_tx_ipv4_cksum_node && offload_tx_ipv4_cksum_node.IsScalar() && (not offload_tx_ipv4_cksum_node.as<bool>())) {
			tx_offloads &= ~DEV_TX_OFFLOAD_IPV4_CKSUM;
		}
		//UDP CKSUM
		YAML::Node offload_tx_udp_cksum_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["tx_udp_cksum"];
		if (offload_tx_udp_cksum_node && offload_tx_udp_cksum_node.IsScalar() && (not offload_tx_udp_cksum_node.as<bool>())) {
			tx_offloads &= ~DEV_TX_OFFLOAD_UDP_CKSUM;
		}
		//TCP CKSUM
		YAML::Node offload_tx_tcp_cksum_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["tx_tcp_cksum"];
		if (offload_tx_tcp_cksum_node && offload_tx_tcp_cksum_node.IsScalar() && (not offload_tx_tcp_cksum_node.as<bool>())) {
			tx_offloads &= ~DEV_TX_OFFLOAD_TCP_CKSUM;
		}
		//SCTP CKSUM
		YAML::Node offload_tx_sctp_cksum_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["tx_sctp_cksum"];
		if (offload_tx_sctp_cksum_node && offload_tx_sctp_cksum_node.IsScalar() && (not offload_tx_sctp_cksum_node.as<bool>())) {
			tx_offloads &= ~DEV_TX_OFFLOAD_SCTP_CKSUM;
		}
		//TCP TSO
		YAML::Node offload_tx_tcp_tso_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["tx_tcp_tso"];
		if (offload_tx_tcp_tso_node && offload_tx_tcp_tso_node.IsScalar() && (not offload_tx_tcp_tso_node.as<bool>())) {
			tx_offloads &= ~DEV_TX_OFFLOAD_TCP_TSO;
		}
		//UDP TSO
		YAML::Node offload_tx_udp_tso_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["tx_udp_tso"];
		if (offload_tx_udp_tso_node && offload_tx_udp_tso_node.IsScalar() && (not offload_tx_udp_tso_node.as<bool>())) {
			tx_offloads &= ~DEV_TX_OFFLOAD_UDP_TSO;
		}
		//OUTER IPV4 CKSUM
		YAML::Node offload_tx_outer_ipv4_cksum_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["tx_outer_ipv4_cksum"];
		if (offload_tx_outer_ipv4_cksum_node && offload_tx_outer_ipv4_cksum_node.IsScalar() && (not offload_tx_outer_ipv4_cksum_node.as<bool>())) {
			tx_offloads &= ~DEV_TX_OFFLOAD_OUTER_IPV4_CKSUM;
		}
		//QINQ INSERT
		YAML::Node offload_tx_qinq_insert_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["tx_qinq_insert"];
		if (offload_tx_qinq_insert_node && offload_tx_qinq_insert_node.IsScalar() && (not offload_tx_qinq_insert_node.as<bool>())) {
			tx_offloads &= ~DEV_TX_OFFLOAD_QINQ_INSERT;
		}
		//VXLAN TNL TSO
		YAML::Node offload_tx_vxlan_tnl_tso_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["tx_vxlan_tnl_tso"];
		if (offload_tx_vxlan_tnl_tso_node && offload_tx_vxlan_tnl_tso_node.IsScalar() && (not offload_tx_vxlan_tnl_tso_node.as<bool>())) {
			tx_offloads &= ~DEV_TX_OFFLOAD_VXLAN_TNL_TSO;
		}
		//GRE TNL TSO
		YAML::Node offload_tx_gre_tnl_tso_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["tx_gre_tnl_tso"];
		if (offload_tx_gre_tnl_tso_node && offload_tx_gre_tnl_tso_node.IsScalar() && (not offload_tx_gre_tnl_tso_node.as<bool>())) {
			tx_offloads &= ~DEV_TX_OFFLOAD_GRE_TNL_TSO;
		}
		//IPIP TNL TSO
		YAML::Node offload_tx_ipip_tnl_tso_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["tx_ipip_tnl_tso"];
		if (offload_tx_ipip_tnl_tso_node && offload_tx_ipip_tnl_tso_node.IsScalar() && (not offload_tx_ipip_tnl_tso_node.as<bool>())) {
			tx_offloads &= ~DEV_TX_OFFLOAD_IPIP_TNL_TSO;
		}
		//GENEVE TNL TSO
		YAML::Node offload_tx_geneve_tnl_tso_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["tx_geneve_tnl_tso"];
		if (offload_tx_geneve_tnl_tso_node && offload_tx_geneve_tnl_tso_node.IsScalar() && (not offload_tx_geneve_tnl_tso_node.as<bool>())) {
			tx_offloads &= ~DEV_TX_OFFLOAD_GENEVE_TNL_TSO;
		}
		//MACSEC INSERT
		YAML::Node offload_tx_macsec_insert_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["offloads"]["tx_macsec_insert"];
		if (offload_tx_macsec_insert_node && offload_tx_macsec_insert_node.IsScalar() && (not offload_tx_macsec_insert_node.as<bool>())) {
			tx_offloads &= ~DEV_TX_OFFLOAD_MACSEC_INSERT;
		}

		//helper string
		std::stringstream s_tx_offloads;
		if (tx_offloads & DEV_TX_OFFLOAD_VLAN_INSERT){
			s_tx_offloads << "TX_VLAN_STRIP, ";
		}
		if (tx_offloads & DEV_TX_OFFLOAD_IPV4_CKSUM){
			s_tx_offloads << "TX_IPV4_CKSUM, ";
		}
		if (tx_offloads & DEV_TX_OFFLOAD_UDP_CKSUM){
			s_tx_offloads << "TX_UDP_CKSUM, ";
		}
		if (tx_offloads & DEV_TX_OFFLOAD_TCP_CKSUM){
			s_tx_offloads << "TX_TCP_CKSUM, ";
		}
		if (tx_offloads & DEV_TX_OFFLOAD_SCTP_CKSUM){
			s_tx_offloads << "TX_SCTP_CKSUM, ";
		}
		if (tx_offloads & DEV_TX_OFFLOAD_TCP_TSO){
			s_tx_offloads << "TX_TCP_TSO, ";
		}
		if (tx_offloads & DEV_TX_OFFLOAD_UDP_TSO){
			s_tx_offloads << "TX_UDP_TSO, ";
		}
		if (tx_offloads & DEV_TX_OFFLOAD_OUTER_IPV4_CKSUM){
			s_tx_offloads << "TX_OUTER_IPV4_CKSUM, ";
		}
		if (tx_offloads & DEV_TX_OFFLOAD_QINQ_INSERT){
			s_tx_offloads << "TX_QINQ_INSERT, ";
		}
		if (tx_offloads & DEV_TX_OFFLOAD_VXLAN_TNL_TSO){
			s_tx_offloads << "TX_VXLAN_TNL_TSO, ";
		}
		if (tx_offloads & DEV_TX_OFFLOAD_GRE_TNL_TSO){
			s_tx_offloads << "TX_GRE_TNL_TSO, ";
		}
		if (tx_offloads & DEV_TX_OFFLOAD_IPIP_TNL_TSO){
			s_tx_offloads << "TX_IPIP_TNL_TSO, ";
		}
		if (tx_offloads & DEV_TX_OFFLOAD_GENEVE_TNL_TSO){
			s_tx_offloads << "TX_GENEVE_TNL_TSO, ";
		}
		if (tx_offloads & DEV_TX_OFFLOAD_MACSEC_INSERT){
			s_tx_offloads << "TX_MACSEC_INSERT, ";
		}

		/*
		 * RSS hash functions
		 */

		uint64_t rss_hf = 0;

		/*
		 * deactivate certain RSS hash functions based on user configuration
		 */

		YAML::Node rss_hf_port_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["rss"]["port"];
		if (rss_hf_port_node && rss_hf_port_node.IsScalar() && (rss_hf_port_node.as<bool>()) && (dev_info.flow_type_rss_offloads & ETH_RSS_PORT)) {
			rss_hf |= ETH_RSS_PORT;
		}

		YAML::Node rss_hf_l2_payload_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["rss"]["l2_payload"];
		if (rss_hf_l2_payload_node && rss_hf_l2_payload_node.IsScalar() && (rss_hf_l2_payload_node.as<bool>()) && (dev_info.flow_type_rss_offloads & ETH_RSS_L2_PAYLOAD)) {
			rss_hf |= ETH_RSS_L2_PAYLOAD;
		}

		YAML::Node rss_hf_ip_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["rss"]["ip"];
		if (rss_hf_ip_node && rss_hf_ip_node.IsScalar() && (rss_hf_ip_node.as<bool>()) && (dev_info.flow_type_rss_offloads & ETH_RSS_IP)) {
			rss_hf |= ETH_RSS_IP;
		}

		YAML::Node rss_hf_udp_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["rss"]["udp"];
		if (rss_hf_udp_node && rss_hf_udp_node.IsScalar() && (rss_hf_udp_node.as<bool>()) && (dev_info.flow_type_rss_offloads & ETH_RSS_UDP)) {
			rss_hf |= ETH_RSS_UDP;
		}

		YAML::Node rss_hf_tcp_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["rss"]["tcp"];
		if (rss_hf_tcp_node && rss_hf_tcp_node.IsScalar() && (rss_hf_tcp_node.as<bool>()) && (dev_info.flow_type_rss_offloads & ETH_RSS_TCP)) {
			rss_hf |= ETH_RSS_TCP;
		}

		YAML::Node rss_hf_sctp_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["rss"]["sctp"];
		if (rss_hf_sctp_node && rss_hf_sctp_node.IsScalar() && (rss_hf_sctp_node.as<bool>()) && (dev_info.flow_type_rss_offloads & ETH_RSS_SCTP)) {
			rss_hf |= ETH_RSS_SCTP;
		}

		YAML::Node rss_hf_tunnel_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["rss"]["tunnel"];
		if (rss_hf_tunnel_node && rss_hf_tunnel_node.IsScalar() && (rss_hf_tunnel_node.as<bool>()) && (dev_info.flow_type_rss_offloads & ETH_RSS_TUNNEL)) {
			rss_hf |= ETH_RSS_TUNNEL;
		}

		YAML::Node rss_hf_vxlan_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["rss"]["vxlan"];
		if (rss_hf_vxlan_node && rss_hf_vxlan_node.IsScalar() && (rss_hf_vxlan_node.as<bool>()) && (dev_info.flow_type_rss_offloads & ETH_RSS_VXLAN)) {
			rss_hf |= ETH_RSS_VXLAN;
		}

		YAML::Node rss_hf_geneve_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["rss"]["geneve"];
		if (rss_hf_geneve_node && rss_hf_geneve_node.IsScalar() && (rss_hf_geneve_node.as<bool>()) && (dev_info.flow_type_rss_offloads & ETH_RSS_GENEVE)) {
			rss_hf |= ETH_RSS_GENEVE;
		}

		YAML::Node rss_hf_nvgre_node = y_config_dpdk_ng["dpdk"]["interfaces"][devname]["ethconf"]["rss"]["nvgre"];
		if (rss_hf_nvgre_node && rss_hf_nvgre_node.IsScalar() && (rss_hf_nvgre_node.as<bool>()) && (dev_info.flow_type_rss_offloads & ETH_RSS_NVGRE)) {
			rss_hf |= ETH_RSS_NVGRE;
		}

		//helper string
		std::stringstream s_rss_hf;
		if (rss_hf & ETH_RSS_PORT){
			s_rss_hf << "ETH_RSS_PORT, ";
		}
		if (rss_hf & ETH_RSS_L2_PAYLOAD){
			s_rss_hf << "ETH_RSS_L2_PAYLOAD, ";
		}
		if (rss_hf & ETH_RSS_IP){
			s_rss_hf << "ETH_RSS_IP, ";
		}
		if (rss_hf & ETH_RSS_UDP){
			s_rss_hf << "ETH_RSS_UDP, ";
		}
		if (rss_hf & ETH_RSS_TCP){
			s_rss_hf << "ETH_RSS_TCP, ";
		}
		if (rss_hf & ETH_RSS_SCTP){
			s_rss_hf << "ETH_RSS_SCTP, ";
		}
		if (rss_hf & ETH_RSS_TUNNEL){
			s_rss_hf << "ETH_RSS_TUNNEL, ";
		}
		if (rss_hf & ETH_RSS_VXLAN){
			s_rss_hf << "ETH_RSS_VXLAN, ";
		}
		if (rss_hf & ETH_RSS_GENEVE){
			s_rss_hf << "ETH_RSS_GENEVE, ";
		}
		if (rss_hf & ETH_RSS_NVGRE){
			s_rss_hf << "ETH_RSS_NVGRE, ";
		}


		//receive side
		eth_conf.link_speeds = ETH_LINK_SPEED_AUTONEG; //auto negotiation enabled
		eth_conf.lpbk_mode = 0; //loopback disabled
		if (rss_hf==0)
			eth_conf.rxmode.mq_mode = ETH_MQ_RX_NONE;
		else
			eth_conf.rxmode.mq_mode = ETH_MQ_RX_RSS; //enable Receive Side Scaling (RSS) only
		eth_conf.rxmode.max_rx_pkt_len = max_rx_pkt_len;
		eth_conf.rxmode.split_hdr_size = 0;
		eth_conf.rxmode.offloads = rx_offloads;
		eth_conf.rxmode.ignore_offload_bitfield = 1;
		eth_conf.rx_adv_conf.rss_conf.rss_key = sym_rss_hash_key;
		eth_conf.rx_adv_conf.rss_conf.rss_key_len = sizeof(sym_rss_hash_key);
		eth_conf.rx_adv_conf.rss_conf.rss_hf = rss_hf;

		//transmit side
		eth_conf.txmode.mq_mode = ETH_MQ_TX_NONE;
		eth_conf.txmode.offloads = tx_offloads;

		XDPD_INFO(DRIVER_NAME"[ifaces][%s] configuring ethdev on physical port: %u, socket: %u, nb-rx-queues: %u, nb-tx-queues: %u, max_rx_pkt_len: %u, rss-hf: 0x%x (caps:0x%x) [%s], rx-offloads: 0x%x (caps:0x%x) [%s], tx-offloads: 0x%x (caps:0x%x) [%s]\n",
				devname.c_str(),
				port_id, socket_id,
				nb_rx_queues,
				nb_tx_queues,
				eth_conf.rxmode.max_rx_pkt_len,
				rss_hf,
				dev_info.flow_type_rss_offloads,
				s_rss_hf.str().c_str(),
				eth_conf.rxmode.offloads,
				dev_info.rx_offload_capa,
				s_rx_offloads.str().c_str(),
				eth_conf.txmode.offloads,
				dev_info.tx_offload_capa,
				s_tx_offloads.str().c_str());

		//configure port
		if ((ret = rte_eth_dev_configure(port_id, nb_rx_queues, nb_tx_queues, &eth_conf)) < 0) {
			switch (ret) {
			case -EINVAL: {
				XDPD_ERR(DRIVER_NAME"[ifaces][%s] failed to configure port %u: rte_eth_dev_configure() (EINVAL)\n", devname.c_str(), port_id);
			} break;
			case -ENOTSUP: {
				XDPD_ERR(DRIVER_NAME"[ifaces][%s] failed to configure port %u: rte_eth_dev_configure() (ENOTSUP)\n", devname.c_str(), port_id);
			} break;
			case -EBUSY: {
				XDPD_ERR(DRIVER_NAME"[ifaces][%s] failed to configure port %u: rte_eth_dev_configure() (EBUSY)\n", devname.c_str(), port_id);
			} break;
			default: {
				XDPD_ERR(DRIVER_NAME"[ifaces][%s] failed to configure port %u: rte_eth_dev_configure()\n", devname.c_str(), port_id);
			};
			}
			XDPD_ERR(DRIVER_NAME"[ifaces][%s] failed to configure port: %u, aborting\n", devname.c_str(), port_id);
			return ROFL_FAILURE;
		}

		// configure transmit queues
		uint16_t nb_tx_desc = dev_info.tx_desc_lim.nb_max / tx_lcores[socket_id].size();
		if (not phyports[port_id].is_virtual && iface_manager_port_setting_exists(s_pci_addr, "nb_tx_desc")) {
			nb_tx_desc = iface_manager_get_port_setting_as<uint16_t>(s_pci_addr, "nb_tx_desc");
		}

		uint64_t tx_prefetch_threshold(TX_PREFETCH_THRESHOLD_DEFAULT);
		if (not phyports[port_id].is_virtual && iface_manager_port_setting_exists(s_pci_addr, "tx_prefetch_threshold")) {
			tx_prefetch_threshold = iface_manager_get_port_setting_as<uint64_t>(s_pci_addr, "tx_prefetch_threshold");
		}

		uint64_t tx_host_threshold(TX_HOST_THRESHOLD_DEFAULT);
		if (not phyports[port_id].is_virtual && iface_manager_port_setting_exists(s_pci_addr, "tx_host_threshold")) {
			tx_host_threshold = iface_manager_get_port_setting_as<uint64_t>(s_pci_addr, "tx_host_threshold");
		}

		uint64_t tx_writeback_threshold(TX_WRITEBACK_THRESHOLD_DEFAULT);
		if (not phyports[port_id].is_virtual && iface_manager_port_setting_exists(s_pci_addr, "tx_writeback_threshold")) {
			tx_writeback_threshold = iface_manager_get_port_setting_as<uint64_t>(s_pci_addr, "tx_writeback_threshold");
		}

		uint64_t tx_free_threshold(TX_FREE_THRESHOLD_DEFAULT);
		if (not phyports[port_id].is_virtual && iface_manager_port_setting_exists(s_pci_addr, "tx_free_threshold")) {
			tx_free_threshold = iface_manager_get_port_setting_as<uint64_t>(s_pci_addr, "tx_free_threshold");
		}

		for (uint16_t tx_queue_id = 0; tx_queue_id < /*no typo!*/nb_tx_queues; tx_queue_id++) {
			struct rte_eth_txconf eth_txconf = dev_info.default_txconf;
#if 0
			if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_I40E_PF, sizeof(DPDK_DRIVER_NAME_I40E_PF)) == 0){

				// values for i40e PF
				eth_txconf.tx_thresh.pthresh = tx_prefetch_threshold;
				eth_txconf.tx_thresh.hthresh = tx_host_threshold;
				eth_txconf.tx_thresh.wthresh = tx_writeback_threshold;
				eth_txconf.tx_free_thresh = tx_free_threshold;
				eth_txconf.tx_rs_thresh = 0; //use default, e.g., I40E_DEFAULT_TX_RSBIT_THRESH = 32
				eth_txconf.tx_deferred_start = 0;
				eth_txconf.txq_flags = ETH_TXQ_FLAGS_NOMULTSEGS;
				eth_txconf.offloads = dev_info.tx_queue_offload_capa;

			} else
			if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_I40E_VF, sizeof(DPDK_DRIVER_NAME_I40E_VF)) == 0){

				// are these values also valid for i40e VF?
				eth_txconf.tx_thresh.pthresh = tx_prefetch_threshold;
				eth_txconf.tx_thresh.hthresh = tx_host_threshold;
				eth_txconf.tx_thresh.wthresh = tx_writeback_threshold;
				eth_txconf.tx_free_thresh = tx_free_threshold;
				eth_txconf.tx_rs_thresh = 0; //use default, e.g., I40E_DEFAULT_TX_RSBIT_THRESH = 32
				eth_txconf.tx_deferred_start = 0;
				eth_txconf.txq_flags = ETH_TXQ_FLAGS_NOMULTSEGS;
				eth_txconf.offloads = dev_info.tx_queue_offload_capa;

			} else if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_IXGBE_PF, sizeof(DPDK_DRIVER_NAME_IXGBE_PF)) == 0) {

				eth_txconf.tx_thresh.pthresh = tx_prefetch_threshold;
				eth_txconf.tx_thresh.hthresh = tx_host_threshold;
				eth_txconf.tx_thresh.wthresh = tx_writeback_threshold;
				eth_txconf.tx_free_thresh = tx_free_threshold;
				eth_txconf.tx_rs_thresh = 0; //use default, e.g., IXGBE_DEFAULT_TX_RSBIT_THRESH = 32
				eth_txconf.tx_deferred_start = 0;
				eth_txconf.txq_flags = ETH_TXQ_FLAGS_NOMULTSEGS;
				eth_txconf.offloads = dev_info.tx_queue_offload_capa;

			} else {

				//defaults for unknown driver
				eth_txconf.tx_thresh.pthresh = tx_prefetch_threshold;
				eth_txconf.tx_thresh.hthresh = tx_host_threshold;
				eth_txconf.tx_thresh.wthresh = tx_writeback_threshold;
				eth_txconf.tx_free_thresh = tx_free_threshold;
				eth_txconf.tx_rs_thresh = 0; //use default, e.g., I40E_DEFAULT_TX_RSBIT_THRESH = 32
				eth_txconf.tx_deferred_start = 0;
				eth_txconf.txq_flags = ETH_TXQ_FLAGS_NOMULTSEGS;
				eth_txconf.offloads = dev_info.tx_queue_offload_capa;

			}
#endif
			XDPD_INFO(DRIVER_NAME"[ifaces][%s] configuring txqueue on physical port: %u, txqueue: %u on socket: %u, nb_tx_desc: %u, tx_prefetch_thresh: %u, tx_host_thresh: %u, tx_writeback_thresh: %u, tx_free_thresh: %u, txq_flags: %u, offloads: 0x%x\n",
					devname.c_str(), port_id, tx_queue_id, socket_id, nb_tx_desc,
					eth_txconf.tx_thresh.pthresh,
					eth_txconf.tx_thresh.hthresh,
					eth_txconf.tx_thresh.wthresh,
					eth_txconf.tx_free_thresh,
					eth_txconf.txq_flags,
					eth_txconf.offloads);

			//configure txqueue
			if (rte_eth_tx_queue_setup(port_id, tx_queue_id, nb_tx_desc, socket_id, &eth_txconf) < 0) {
				XDPD_ERR(DRIVER_NAME" Failed to configure port: %u tx-queue: %u, aborting\n", port_id, tx_queue_id);
				return ROFL_FAILURE;
			}
		}


		// configure receive queues
		uint16_t nb_rx_desc = dev_info.rx_desc_lim.nb_max / rx_lcores[socket_id].size();
		if (not phyports[port_id].is_virtual && iface_manager_port_setting_exists(s_pci_addr, "nb_rx_desc")) {
			nb_rx_desc = iface_manager_get_port_setting_as<uint16_t>(s_pci_addr, "nb_rx_desc");
		}

		uint64_t rx_prefetch_threshold(RX_PREFETCH_THRESHOLD_DEFAULT);
		if (not phyports[port_id].is_virtual && iface_manager_port_setting_exists(s_pci_addr, "rx_prefetch_threshold")) {
			rx_prefetch_threshold = iface_manager_get_port_setting_as<uint64_t>(s_pci_addr, "rx_prefetch_threshold");
		}

		uint64_t rx_host_threshold(RX_HOST_THRESHOLD_DEFAULT);
		if (not phyports[port_id].is_virtual && iface_manager_port_setting_exists(s_pci_addr, "rx_host_threshold")) {
			rx_host_threshold = iface_manager_get_port_setting_as<uint64_t>(s_pci_addr, "rx_host_threshold");
		}

		uint64_t rx_writeback_threshold(RX_WRITEBACK_THRESHOLD_DEFAULT);
		if (not phyports[port_id].is_virtual && iface_manager_port_setting_exists(s_pci_addr, "rx_writeback_threshold")) {
			rx_writeback_threshold = iface_manager_get_port_setting_as<uint64_t>(s_pci_addr, "rx_writeback_threshold");
		}

		uint64_t rx_free_threshold(RX_FREE_THRESHOLD_DEFAULT);
		if (not phyports[port_id].is_virtual && iface_manager_port_setting_exists(s_pci_addr, "rx_free_threshold")) {
			rx_free_threshold = iface_manager_get_port_setting_as<uint64_t>(s_pci_addr, "rx_free_threshold");
		}

		for (uint16_t rx_queue_id = 0; rx_queue_id < nb_rx_queues; rx_queue_id++) {
			struct rte_eth_rxconf eth_rxconf = dev_info.default_rxconf;
#if 0
			if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_I40E_PF, sizeof(DPDK_DRIVER_NAME_I40E_PF)) == 0){

				// values for i40e PF
				eth_rxconf.rx_thresh.pthresh = rx_prefetch_threshold;
				eth_rxconf.rx_thresh.hthresh = rx_host_threshold;
				eth_rxconf.rx_thresh.wthresh = rx_writeback_threshold;
				eth_rxconf.rx_drop_en = 1; //drop packets when descriptor space is exhausted
				eth_rxconf.rx_free_thresh = rx_free_threshold;
				eth_rxconf.rx_deferred_start = 0;
				eth_rxconf.offloads = dev_info.rx_queue_offload_capa;

			} else
			if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_I40E_VF, sizeof(DPDK_DRIVER_NAME_I40E_VF)) == 0){

				// are these values also valid for i40e VF?
				eth_rxconf.rx_thresh.pthresh = rx_prefetch_threshold;
				eth_rxconf.rx_thresh.hthresh = rx_host_threshold;
				eth_rxconf.rx_thresh.wthresh = rx_writeback_threshold;
				eth_rxconf.rx_drop_en = 1; //drop packets when descriptor space is exhausted
				eth_rxconf.rx_free_thresh = rx_free_threshold;
				eth_rxconf.rx_deferred_start = 0;
				eth_rxconf.offloads = dev_info.rx_queue_offload_capa;

			} else if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_IXGBE_PF, sizeof(DPDK_DRIVER_NAME_IXGBE_PF)) == 0) {

				eth_rxconf.rx_thresh.pthresh = rx_prefetch_threshold;
				eth_rxconf.rx_thresh.hthresh = rx_host_threshold;
				eth_rxconf.rx_thresh.wthresh = rx_writeback_threshold;
				eth_rxconf.rx_drop_en = 1; //drop packets when descriptor space is exhausted
				eth_rxconf.rx_free_thresh = rx_free_threshold;
				eth_rxconf.rx_deferred_start = 0;
				eth_rxconf.offloads = dev_info.rx_queue_offload_capa;

			} else {

				//defaults for unknown driver
				eth_rxconf.rx_thresh.pthresh = rx_prefetch_threshold;
				eth_rxconf.rx_thresh.hthresh = rx_host_threshold;
				eth_rxconf.rx_thresh.wthresh = rx_writeback_threshold;
				eth_rxconf.rx_free_thresh = rx_free_threshold;
				eth_rxconf.rx_deferred_start = 0;
				eth_rxconf.offloads = dev_info.rx_queue_offload_capa;

			}
#endif
			XDPD_INFO(DRIVER_NAME"[ifaces][%s] configuring rxqueue on physical port: %u, rxqueue: %u on socket: %u, nb_rx_desc: %u, rx_prefetch_thresh: %u, rx_host_thresh: %u, rx_writeback_thresh: %u, rx_free_thresh: %u, offloads: 0x%x\n",
					devname.c_str(), port_id, rx_queue_id, socket_id, nb_rx_desc,
					eth_rxconf.rx_thresh.pthresh,
					eth_rxconf.rx_thresh.hthresh,
					eth_rxconf.rx_thresh.wthresh,
					eth_rxconf.rx_free_thresh,
					eth_rxconf.offloads);

			//configure rxqueue
			if (rte_eth_rx_queue_setup(port_id, rx_queue_id, nb_rx_desc, socket_id, &eth_rxconf, direct_pools[socket_id]) < 0) {
				XDPD_ERR(DRIVER_NAME" failed to configure port: %u rx-queue: %u, aborting\n", port_id, rx_queue_id);
				return ROFL_FAILURE;
			}
		}
	}

	//configure physical functions
	for (uint16_t port_id = 0; port_id < rte_eth_dev_count(); port_id++) {

		if (not phyports[port_id].is_enabled) {
			continue;
		}

		if (phyports[port_id].is_vf) {
			continue;
		}

		if (phyports[port_id].is_virtual) {
			continue;
		}

		rte_eth_dev_info_get(port_id, &dev_info);
		if (dev_info.pci_dev) {
			memset(s_pci_addr, 0, sizeof(s_pci_addr));
			rte_pci_device_name(&(dev_info.pci_dev->addr), s_pci_addr, sizeof(s_pci_addr));
		}

		//configure MAC addresses
		node = iface_manager_port_conf(s_pci_addr)["mac_addr"];
		if (node && node.IsSequence()) {
			int index = 0;
			for (auto it : node) {
				struct ether_addr eth_addr;
				sscanf(it.as<std::string>().c_str(),
						"%02" SCNx8 ":%02" SCNx8 ":%02" SCNx8 ":%02" SCNx8 ":%02" SCNx8 ":%02" SCNx8,
							&eth_addr.addr_bytes[0],
							&eth_addr.addr_bytes[1],
							&eth_addr.addr_bytes[2],
							&eth_addr.addr_bytes[3],
							&eth_addr.addr_bytes[4],
							&eth_addr.addr_bytes[5]);
				XDPD_INFO(DRIVER_NAME" adding mac-address: %s on port: %u\n", it.as<std::string>().c_str(), port_id);
				if (index == 0) {
					if ((ret = rte_eth_dev_default_mac_addr_set(port_id, &eth_addr)) < 0) {
						XDPD_ERR(DRIVER_NAME" failed to configure first mac-address: %s on port: %u, aborting\n", it.as<std::string>().c_str(), port_id);
						//return ROFL_FAILURE;
					}
				} else {
					if ((ret = rte_eth_dev_mac_addr_add(port_id, &eth_addr, 0)) < 0) {
						XDPD_ERR(DRIVER_NAME" failed to configure additional mac-address: %s on port: %u, aborting\n", it.as<std::string>().c_str(), port_id);
						//return ROFL_FAILURE;
					}
				}
				++index;
			}
		}

		//configure VLAN filters for virtual functions
		node = iface_manager_port_conf(s_pci_addr)["vlan_filter"];
		if (node && node.IsSequence()) {
			int index = 0;
			for (auto filter : node) {
				if (not filter["vlan_id"]) {
					XDPD_INFO(DRIVER_NAME" skipping vlan filter for port: %u, no \"vlan_id\" specified\n", port_id);
					continue;
				}
				if (not filter["vf_mask"]) {
					XDPD_INFO(DRIVER_NAME" skipping vlan filter for port: %u, no \"vf_mask\" specified\n", port_id);
					continue;
				}
				uint16_t vlan_id = filter["vlan_id"].as<uint16_t>();
				uint64_t vf_mask = filter["vf_mask"].as<uint64_t>();
				uint8_t on = 1; //default: enabled
				if (filter["enabled"]) {
					on = filter["enabled"].as<bool>();
				}
				XDPD_INFO(DRIVER_NAME" adding vlan filter with vlan_id: %u and vf_mask: 0x%llx on port: %u\n", vlan_id, vf_mask, port_id);
				if ((ret = set_vf_vlan_filter(port_id, vlan_id, vf_mask, on)) < 0) {
					XDPD_ERR(DRIVER_NAME" failed to configure vlan filter with vlan_id: %u and vf_mask: 0x%llx on port: %u\n", vlan_id, vf_mask, port_id);
					//return ROFL_FAILURE;
				}
				++index;
			}
		}

		//configure promisc
		node = iface_manager_port_conf(s_pci_addr)["promisc"];
		if (node && node.IsScalar()) {
			bool on = node.as<bool>();
			XDPD_INFO(DRIVER_NAME" setting promisc: %s on port: %u\n", (on ? "yes":"no"), port_id);
			set_promisc(port_id, on);
		}

		//configure allmulticast
		node = iface_manager_port_conf(s_pci_addr)["allmulticast"];
		if (node && node.IsScalar()) {
			bool on = node.as<bool>();
			XDPD_INFO(DRIVER_NAME" setting allmulticast: %s on port: %u\n", (on ? "yes":"no"), port_id);
			set_allmulticast(port_id, on);
		}

		//configure tx loopback
		node = iface_manager_port_conf(s_pci_addr)["tx_loopback"];
		if (node && node.IsScalar()) {
			bool on = node.as<bool>();
			XDPD_INFO(DRIVER_NAME" setting tx-loopback: %s on port: %u\n", (on ? "yes":"no"), port_id);
			if ((ret = set_tx_loopback(port_id, on)) < 0) {
				XDPD_ERR(DRIVER_NAME" failed to configure tx-loopback: %s on port: %u, aborting\n", (on ? "yes":"no"), port_id);
				//return ROFL_FAILURE;
			}
		}
	}

	//configure virtual functions
	for (uint16_t port_id = 0; port_id < rte_eth_dev_count(); port_id++) {

		if (not phyports[port_id].is_enabled) {
			continue;
		}

		if (not phyports[port_id].is_vf) {
			continue;
		}

		if (phyports[port_id].is_virtual) {
			continue;
		}

		uint16_t vf_id = phyports[port_id].vf_id;

		rte_eth_dev_info_get(port_id, &dev_info);
		if (dev_info.pci_dev) {
			memset(s_pci_addr, 0, sizeof(s_pci_addr));
			rte_pci_device_name(&(dev_info.pci_dev->addr), s_pci_addr, sizeof(s_pci_addr));
		}

		//configure MAC addresses
		node = iface_manager_port_conf(s_pci_addr)["mac_addr"];
		if (node && node.IsSequence()) {
			int index = 0;
			for (auto it : node) {
				struct ether_addr eth_addr;
				sscanf(it.as<std::string>().c_str(),
						"%02" SCNx8 ":%02" SCNx8 ":%02" SCNx8 ":%02" SCNx8 ":%02" SCNx8 ":%02" SCNx8,
							&eth_addr.addr_bytes[0],
							&eth_addr.addr_bytes[1],
							&eth_addr.addr_bytes[2],
							&eth_addr.addr_bytes[3],
							&eth_addr.addr_bytes[4],
							&eth_addr.addr_bytes[5]);
				XDPD_INFO(DRIVER_NAME" adding mac-address: %s on port: %u, parent port: %u, vf_id: %u\n", it.as<std::string>().c_str(), port_id, phyports[port_id].parent_port_id, vf_id);
				if (index == 0) {
					if ((ret = set_vf_mac_addr(phyports[port_id].parent_port_id, vf_id, &eth_addr)) < 0) {
						XDPD_ERR(DRIVER_NAME" failed to configure first mac-address: %s on port: %u, aborting\n", it.as<std::string>().c_str(), port_id);
						//return ROFL_FAILURE;
					}
				} else {
					if ((ret = add_vf_mac_addr(phyports[port_id].parent_port_id, vf_id, &eth_addr)) < 0) {
						XDPD_ERR(DRIVER_NAME" failed to configure additional mac-address: %s on port: %u, aborting\n", it.as<std::string>().c_str(), port_id);
						//return ROFL_FAILURE;
					}
				}
				++index;
			}
		}

		//configure MAC anti spoof
		node = iface_manager_port_conf(s_pci_addr)["mac_anti_spoof"];
		if (node && node.IsScalar()) {
			bool on = node.as<bool>();
			XDPD_INFO(DRIVER_NAME" setting mac-anti-spoof: %s on port: %u, parent port: %u, vf_id: %u\n", (on ? "yes":"no"), port_id, phyports[port_id].parent_port_id, vf_id);
			if ((ret = set_vf_mac_anti_spoof(phyports[port_id].parent_port_id, vf_id, on)) < 0) {
				XDPD_ERR(DRIVER_NAME" failed to configure mac-anti-spoof: %s on port: %u, aborting\n", (on ? "yes":"no"), port_id);
				//return ROFL_FAILURE;
			}
		}

		//configure VLAN anti spoof
		node = iface_manager_port_conf(s_pci_addr)["vlan_anti_spoof"];
		if (node && node.IsScalar()) {
			bool on = node.as<bool>();
			XDPD_INFO(DRIVER_NAME" setting vlan-anti-spoof: %s on port: %u, parent port: %u, vf_id: %u\n", (on ? "yes":"no"), port_id, phyports[port_id].parent_port_id, vf_id);
			if ((ret = set_vf_vlan_anti_spoof(phyports[port_id].parent_port_id, vf_id, on)) < 0) {
				XDPD_ERR(DRIVER_NAME" failed to configure vlan-anti-spoof: %s on port: %u, aborting\n", (on ? "yes":"no"), port_id);
				//return ROFL_FAILURE;
			}
		}

		//configure unicast promisc
		node = iface_manager_port_conf(s_pci_addr)["unicast_promisc"];
		if (node && node.IsScalar()) {
			bool on = node.as<bool>();
			XDPD_INFO(DRIVER_NAME" setting unicast-promisc: %s on port: %u, parent port: %u, vf_id: %u\n", (on ? "yes":"no"), port_id, phyports[port_id].parent_port_id, vf_id);
			if ((ret = set_vf_unicast_promisc(phyports[port_id].parent_port_id, vf_id, on)) < 0) {
				XDPD_ERR(DRIVER_NAME" failed to configure unicast-promisc: %s on port: %u, aborting\n", (on ? "yes":"no"), port_id);
				//return ROFL_FAILURE;
			}
		}

		//configure multicast promisc
		node = iface_manager_port_conf(s_pci_addr)["multicast_promisc"];
		if (node && node.IsScalar()) {
			bool on = node.as<bool>();
			XDPD_INFO(DRIVER_NAME" setting multicast-promisc: %s on port: %u, parent port: %u, vf_id: %u\n", (on ? "yes":"no"), port_id, phyports[port_id].parent_port_id, vf_id);
			if ((ret = set_vf_multicast_promisc(phyports[port_id].parent_port_id, vf_id, on)) < 0) {
				XDPD_ERR(DRIVER_NAME" failed to configure multicast-promisc: %s on port: %u, aborting\n", (on ? "yes":"no"), port_id);
				//return ROFL_FAILURE;
			}
		}

		//configure broadcast
		node = iface_manager_port_conf(s_pci_addr)["broadcast"];
		if (node && node.IsScalar()) {
			bool on = node.as<bool>();
			XDPD_INFO(DRIVER_NAME" setting broadcast: %s on port: %u, parent port: %u, vf_id: %u\n", (on ? "yes":"no"), port_id, phyports[port_id].parent_port_id, vf_id);
			if ((ret = set_vf_broadcast(phyports[port_id].parent_port_id, vf_id, on)) < 0) {
				XDPD_ERR(DRIVER_NAME" failed to configure broadcast: %s on port: %u, aborting\n", (on ? "yes":"no"), port_id);
				//return ROFL_FAILURE;
			}
		}

		//configure vlan stripq
		node = iface_manager_port_conf(s_pci_addr)["vlan_stripq"];
		if (node && node.IsScalar()) {
			bool on = node.as<bool>();
			XDPD_INFO(DRIVER_NAME" setting vlan-stripq: %s on port: %u, parent port: %u, vf_id: %u\n", (on ? "yes":"no"), port_id, phyports[port_id].parent_port_id, vf_id);
			if ((ret = set_vf_vlan_stripq(phyports[port_id].parent_port_id, vf_id, on)) < 0) {
				XDPD_ERR(DRIVER_NAME" failed to configure vlan-stripq: %s on port: %u, aborting\n", (on ? "yes":"no"), port_id);
				//return ROFL_FAILURE;
			}
		}

		//configure vlan insert
		node = iface_manager_port_conf(s_pci_addr)["vlan_insert"];
		if (node && node.IsScalar()) {
			uint16_t vlan_id = node.as<uint16_t>();
			XDPD_INFO(DRIVER_NAME" setting vlan-insert: %u on port: %u, parent port: %u, vf_id: %u\n", vlan_id, port_id, phyports[port_id].parent_port_id, vf_id);
			if ((ret = set_vf_vlan_insert(phyports[port_id].parent_port_id, vf_id, vlan_id)) < 0) {
				XDPD_ERR(DRIVER_NAME" failed to configure vlan-insert: %u on port: %u, aborting\n", vlan_id, port_id);
				//return ROFL_FAILURE;
			}
		}

		//configure vlan tag
		node = iface_manager_port_conf(s_pci_addr)["vlan_tag"];
		if (node && node.IsScalar()) {
			bool on = node.as<bool>();
			XDPD_INFO(DRIVER_NAME" setting vlan-tag: %s on port: %u, parent port: %u, vf_id: %u\n", (on ? "yes":"no"), port_id, phyports[port_id].parent_port_id, vf_id);
			if ((ret = set_vf_vlan_tag(phyports[port_id].parent_port_id, vf_id, on)) < 0) {
				XDPD_ERR(DRIVER_NAME" failed to configure vlan-tag: %s on port: %u, aborting\n", (on ? "yes":"no"), port_id);
				//return ROFL_FAILURE;
			}
		}

		++vf_id;
	}

	//configure arbitrary stuff
	for (uint16_t port_id = 0; port_id < rte_eth_dev_count(); port_id++) {
		rte_eth_dev_info_get(port_id, &dev_info);
		if (dev_info.pci_dev) {
			memset(s_pci_addr, 0, sizeof(s_pci_addr));
			rte_pci_device_name(&(dev_info.pci_dev->addr), s_pci_addr, sizeof(s_pci_addr));
		}

		//shortcut port configuration for testing
		if (iface_manager_port_setting_exists(s_pci_addr, "shortcut") && iface_manager_port_exists(iface_manager_get_port_setting_as<std::string>(s_pci_addr, "shortcut"))) {
			phyports[port_id].shortcut_port_id = iface_manager_pci_address_to_port_id(iface_manager_get_port_setting_as<std::string>(s_pci_addr, "shortcut"));
		} else {
			phyports[port_id].shortcut_port_id = port_id; //sent packet out via in-port, if no shortcut port was defined
		}
	}

	//unsigned int vport_name_index = 0;
	//Iterate over all available physical ports
	for (uint16_t port_id = 0; port_id < rte_eth_dev_count(); port_id++) {
		char port_name[SWITCH_PORT_MAX_LEN_NAME];
		switch_port_t* port;
		int socket_id;

		if (not phyports[port_id].is_enabled) {
			continue;
		}

		char ifname[IF_NAMESIZE];
		if ((ret = rte_eth_dev_get_name_by_port(port_id, ifname)) < 0) {
			continue;
		}

		rte_eth_dev_info_get(port_id, &dev_info);

		/* net_kni PMD */
		if (dev_info.driver_name == std::string("net_kni")) {
			//snprintf (port_name, SWITCH_PORT_MAX_LEN_NAME, vport_names[vport_name_index++]);
			snprintf (port_name, SWITCH_PORT_MAX_LEN_NAME, ifname+8); //strip off "net_"
		} else
		/* net_pcap PMD */
		if (dev_info.driver_name == std::string("net_pcap")) {
			//snprintf (port_name, SWITCH_PORT_MAX_LEN_NAME, vport_names[vport_name_index++]);
			snprintf (port_name, SWITCH_PORT_MAX_LEN_NAME, ifname+9); //strip off "net_"
		} else
		/* net_ring PMD */
		if (dev_info.driver_name == std::string("net_ring")) {
			//snprintf (port_name, SWITCH_PORT_MAX_LEN_NAME, vport_names[vport_name_index++]);
			snprintf (port_name, SWITCH_PORT_MAX_LEN_NAME, ifname+9);
		} else
		/* physical ports */
		if (true) {
			memset(s_pci_addr, 0, sizeof(s_pci_addr));
			if (dev_info.pci_dev) {
				rte_pci_device_name(&(dev_info.pci_dev->addr), s_pci_addr, sizeof(s_pci_addr));
			}
			snprintf (port_name, SWITCH_PORT_MAX_LEN_NAME, iface_manager_get_port_setting_as<std::string>(s_pci_addr, "ifname").c_str());
		}

		socket_id = phyports[port_id].socket_id;

		XDPD_INFO(DRIVER_NAME"[ifaces][%s] adding xdpd port: %s for dpdk port: %u on socket: %u\n", port_name, port_name, port_id, socket_id);

		//Initialize pipeline port
		port = switch_port_init(port_name, false, PORT_TYPE_PHYSICAL, PORT_STATE_NONE);
		if(!port){
			XDPD_ERR(DRIVER_NAME" failed to create xdpd port: %s for dpdk port: %u\n", port_name, port_id);
			return ROFL_FAILURE;
		}

		//Generate port state
		dpdk_port_state_t* ps = (dpdk_port_state_t*)rte_malloc(NULL,sizeof(dpdk_port_state_t),0);

		if(!ps){
			XDPD_ERR(DRIVER_NAME" unable to allocate memory for xdpd switch_port_t: %s\n", port_name);
			switch_port_destroy(port);
			return ROFL_FAILURE;
		}

		//Fill-in dpdk port state
		ps->queues_set = false;
		ps->scheduled = false;
		ps->port_id = port_id;
		ps->socket_id = socket_id;
		port->platform_port_state = (platform_port_state_t*)ps;

		//Set the port in the phy_port_mapping
		phy_port_mapping[port_id] = port;

		//Add port to the pipeline
		if( physical_switch_add_port(port) != ROFL_SUCCESS ){
			XDPD_ERR(DRIVER_NAME"[iface_manager] Unable to add the switch port to physical switch; perhaps there are no more physical port slots available?\n");
			return ROFL_FAILURE;
		}
	}

	return ROFL_SUCCESS;
}

/*
* Discovers and initializes (including rofl-pipeline state) DPDK-enabled ports.
*/
rofl_result_t iface_manager_discover_system_ports(void){

	if (iface_manager_setup_virtual_ports() < 0) {
		XDPD_ERR(DRIVER_NAME"[iface_manager] iface_manager_setup_virtual_ports failed\n");
		return ROFL_FAILURE;
	}

	if (iface_manager_discover_physical_ports() < 0) {
		XDPD_ERR(DRIVER_NAME"[iface_manager] iface_manager_discover_physical_ports failed\n");
		return ROFL_FAILURE;
	}

	return ROFL_SUCCESS;
}

/*
* Creates a virtual link port pair. TODO: this function is not thread safe
*/
rofl_result_t iface_manager_create_virtual_port_pair(of_switch_t* lsw1, switch_port_t **vport1, of_switch_t* lsw2, switch_port_t **vport2){

	//Not supported on dpdk-ng driver. Use a pair of ring based ethernet devices and the associated net_ring PMD instead.
	return ROFL_FAILURE;
}



/*
* Enable port 
*/
rofl_result_t iface_manager_bring_up(switch_port_t* port){

	unsigned int port_id;
	int ret;
	
	if(unlikely(!port))
		return ROFL_FAILURE;

	if(port->type == PORT_TYPE_VIRTUAL)
	{
		/*
		* Virtual link
		*/
	}else{
		/*
		*  PHYSICAL
		*/
		port_id = ((dpdk_port_state_t*)port->platform_port_state)->port_id;

		//Start port in RTE
		if(!port->up){
			//Was down; simply start
			if((ret=rte_eth_dev_start(port_id)) < 0){
				XDPD_ERR(DRIVER_NAME"[iface_manager] Cannot start device %u:  %s\n", port_id, rte_strerror(ret));
				assert(0);
				return ROFL_FAILURE; 
			}
		}
	}
		
	//Mark the port as being up and return
	port->up = true;
		
	return ROFL_SUCCESS;
}

/*
* Disable port 
*/
rofl_result_t iface_manager_bring_down(switch_port_t* port){

	unsigned int port_id;
	
	if(unlikely(!port))
		return ROFL_FAILURE;
	
	if(port->type == PORT_TYPE_VIRTUAL) {
		/*
		* Virtual link
		*/
	}else {
		/*
		*  PHYSICAL
		*/

		port_id = ((dpdk_port_state_t*)port->platform_port_state)->port_id;

		//First mark the port as NOT up, so that cores don't issue
		//RX/TX calls over the port
		port->up = false;

		//Stop port in RTE
		if(port->up){
			//Was  up; stop it
			rte_eth_dev_stop(port_id);
		}
	}

	return ROFL_SUCCESS;
}


/*
* Shutdown all ports in the system 
*/
rofl_result_t iface_manager_destroy(void){

	uint8_t i, num_of_ports;
	num_of_ports = rte_eth_dev_count();
	
	for(i=0;i<num_of_ports;++i){
		rte_eth_dev_stop(i);
		rte_eth_dev_close(i);
		//IVANO - TODO: destroy also NF ports
	}	

	return ROFL_SUCCESS;
}

/*
* Update link states 
*/
void iface_manager_update_links(){

	unsigned int i;
	struct rte_eth_link link;
	switch_port_t* port;
	switch_port_snapshot_t* port_snapshot;
	bool last_link_state;
	
	for(i=0;i<PORT_MANAGER_MAX_PORTS;i++){
		
		port = phy_port_mapping[i];
		
		if(unlikely(port != NULL)){
			rte_eth_link_get_nowait(i,&link);
	
			last_link_state = !((port->state& PORT_STATE_LINK_DOWN) > 0); //up =>1

			//Check if there has been a change
			if(unlikely(last_link_state != link.link_status)){
				if(link.link_status)
					//Up
					port->state = port->state & ~(PORT_STATE_LINK_DOWN); 
				else
					//Down
					port->state = port->state | PORT_STATE_LINK_DOWN;
					
				XDPD_DEBUG(DRIVER_NAME"[port-manager] Port %s is %s, and link is %s\n", port->name, ((port->up) ? "up" : "down"), ((link.link_status) ? "detected" : "not detected"));
				
				//Notify CMM port change
				port_snapshot = physical_switch_get_port_snapshot(port->name); 
				if(hal_cmm_notify_port_status_changed(port_snapshot) != HAL_SUCCESS){
					XDPD_DEBUG(DRIVER_NAME"[iface_manager] Unable to notify port status change for port %s\n", port->name);
				}	
			}
		}
	}
}

/*
* Update port stats (pipeline)
*/
void iface_manager_update_stats(){

	unsigned int i, j;
	struct rte_eth_stats stats;
	switch_port_t* port;

	for(i=0; i<PORT_MANAGER_MAX_PORTS; ++i){

		port = phy_port_mapping[i];

		if(!port)
			continue;

		//Retrieve stats
		rte_eth_stats_get(i, &stats);

		//RX
		port->stats.rx_packets = stats.ipackets;
		port->stats.rx_bytes = stats.ibytes;
		port->stats.rx_errors = stats.ierrors;

		//FIXME: collisions and other errors

		//TX
		port->stats.tx_packets = stats.opackets;
		port->stats.tx_bytes = stats.obytes;
		port->stats.tx_errors = stats.oerrors;

		//TX-queues
		for(j=0; j<IO_IFACE_NUM_QUEUES; ++j){
			port->queues[j].stats.tx_packets = stats.q_opackets[j];
			port->queues[j].stats.tx_bytes = stats.q_obytes[j];
			//port->queues[j].stats.overrun = stats.q_;
		}
	}

}

