#include "iface_manager.h"
#include <math.h>
#include <rofl/datapath/hal/cmm.h>
#include <utils/c_logger.h>
#include <rofl/datapath/pipeline/openflow/of_switch.h>
#include <rofl/datapath/pipeline/common/datapacket.h>
#include <rofl/datapath/pipeline/physical_switch.h>

#include "port_state.h"
#include "nf_iface_manager.h"

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

#define NB_MBUF                                                                                                        \
	RTE_MAX((nb_ports * nb_rx_queue * RTE_RX_DESC_DEFAULT + nb_ports * nb_lcores * IO_IFACE_MAX_PKT_BURST +        \
		 nb_ports * n_tx_queue * RTE_TX_DESC_DEFAULT + nb_lcores * MEMPOOL_CACHE_SIZE),                        \
		(unsigned)8192)

#define MEMPOOL_CACHE_SIZE 256

#define VLAN_RX_FILTER
//#define VLAN_SET_MACVLAN_FILTER
//#define USE_INPUT_FILTER_SET

struct ether_addr ports_eth_addr[RTE_MAX_ETHPORTS];

switch_port_t* phy_port_mapping[PORT_MANAGER_MAX_PORTS] = {0};
struct rte_ring* port_tx_lcore_queue[PORT_MANAGER_MAX_PORTS][IO_IFACE_NUM_QUEUES] = {{NULL}}; // XXX(toanju) should be sufficient for shmen only

uint8_t nb_phy_ports = 0;
pthread_rwlock_t iface_manager_rwlock = PTHREAD_RWLOCK_INITIALIZER;

/* Static global variables used within this file. */
//static uint16_t nb_rxd = RTE_RX_DESC_DEFAULT;
uint16_t nb_txd = RTE_TX_DESC_DEFAULT;
uint16_t nb_rxd = RTE_RX_DESC_DEFAULT;

//a set of available NUMA sockets (socket_id)
static std::set<int> sockets;

/* a map of available event logical cores per NUMA socket (set of lcore_id) */
extern std::map<unsigned int, std::set<unsigned int> > svc_lcores;
/* a map of available event logical cores per NUMA socket (set of lcore_id) */
extern std::map<unsigned int, std::set<unsigned int> > ev_lcores;
/* a map of available RX logical cores per NUMA socket (set of lcore_id) */
extern std::map<unsigned int, std::set<unsigned int> > rx_lcores;
/* a map of available TX logical cores per NUMA socket (set of lcore_id) */
extern std::map<unsigned int, std::set<unsigned int> > tx_lcores;
/* a map of available worker logical cores per NUMA socket (set of lcore_id) */
extern std::map<unsigned int, std::set<unsigned int> > wk_lcores;




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

uint8_t get_port_n_rx_queues(const uint8_t port)
{
	int queue = -1;
	uint16_t i;

	for (i = 0; i < nb_lcore_params; ++i) {
		if (lcore_params[i].port_id == port) {
			if (lcore_params[i].queue_id == queue+1)
				queue = lcore_params[i].queue_id;
			else
				rte_exit(EXIT_FAILURE, "queue ids of the port %d must be"
						" in sequence and must start with 0\n",
						lcore_params[i].port_id);
		}
	}
	return (uint8_t)(++queue);
}

uint8_t get_port_n_tx_queues(const uint8_t lsi_id, const uint8_t port)
{
	int queue_cnt[RTE_MAX_LCORE];
	uint16_t i;
	uint8_t lcore_id;
	int queue = 0;

	memset(&queue_cnt, 0, sizeof(queue_cnt));

	for (i = 0; i < nb_lcore_params; ++i) {
		if (lcore_params[i].lsi_id == lsi_id && lcore_params[i].port_id != port)
			queue_cnt[lcore_params[i].lcore_id]++;
	}

	for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
		if (queue_cnt[lcore_id])
			queue++;
	}

	if (queue > MAX_TX_QUEUE_PER_PORT) {
		rte_exit(EXIT_FAILURE, "too many tx queues for port %d: %d\n", port, queue);
	}

	return (uint8_t)(queue);
}

uint8_t get_lsi_id(const uint8_t port_id) {
	unsigned i;

	for (i = 0; i < nb_lcore_params; ++i) {
		if (lcore_params[i].port_id == port_id) {
			return lcore_params[i].lsi_id;
		}
	}

	return -1;
}

unsigned is_txq_enabled(const uint8_t lsi_id, const uint8_t port_id, const uint8_t lcore_id)
{
	unsigned i;

	for (i = 0; i < nb_lcore_params; ++i) {
		if (lcore_params[i].lsi_id == lsi_id && lcore_params[i].port_id != port_id &&
		    lcore_params[i].lcore_id == lcore_id) {
			return 1;
		}
	}

	return 0;
}

#if 0
static int check_lcore_params(void)
{
	uint8_t queue, lcore;
	uint16_t i;
	int socketid;

	for (i = 0; i < nb_lcore_params; ++i) {
		queue = lcore_params[i].queue_id;
		if (queue >= MAX_RX_QUEUE_PER_PORT) {
			XDPD_ERR("invalid queue number: %hhu\n", queue);
			return -1;
		}
		lcore = lcore_params[i].lcore_id;
		if (!rte_lcore_is_enabled(lcore)) {
			XDPD_ERR("error: lcore %hhu is not enabled in lcore mask\n", lcore);
			return -1;
		}
		if ((socketid = rte_lcore_to_socket_id(lcore) != 0) &&
		    (numa_on == 0)) {
			XDPD_WARN("warning: lcore %hhu is on socket %d with numa "
				  "off \n",
				  lcore, socketid);
		}
		sockets.insert(socketid);
	}
	return 0;
}
#endif
#if 0
static int
init_lcore_rx_queues(void)
{
        uint16_t i, nb_rx_queue;
        uint8_t lcore;

        for (i = 0; i < nb_lcore_params; ++i) {
                lcore = lcore_params[i].lcore_id;
                nb_rx_queue = wk_core_tasks[lcore].n_rx_queue;
                if (nb_rx_queue >= MAX_RX_QUEUE_PER_LCORE) {
                        XDPD_ERR("error: too many queues (%u) for lcore: %u\n",
                                (unsigned)nb_rx_queue + 1, (unsigned)lcore);
                        return -1;
                } else {
                        wk_core_tasks[lcore].rx_queue_list[nb_rx_queue].port_id =
                                lcore_params[i].port_id; // XXX(toanju) this is currently pretty static wrt. port_id
                        wk_core_tasks[lcore].rx_queue_list[nb_rx_queue].queue_id =
                                lcore_params[i].queue_id;
                        wk_core_tasks[lcore].n_rx_queue++;
                }
		XDPD_INFO("init_lcore_rx_queue i=%d lcore=%d #lcore_queues=%d\n", i, lcore, wk_core_tasks[lcore].n_rx_queue);
        }
        return 0;
}
#endif
#if 0
static int check_port_config(const unsigned nb_ports)
{
	unsigned portid;
	uint16_t i;

	for (i = 0; i < nb_lcore_params; ++i) {
		portid = lcore_params[i].port_id;
		if (portid >= nb_ports + GNU_LINUX_DPDK_MAX_KNI_IFACES) {
			XDPD_ERR("port %u is not present on the board\n", portid);
			return -1;
		}
	}
	return 0;
}
#endif
#if 0
static int init_mem(unsigned nb_mbuf)
{
	int socketid;
	unsigned lcore_id;
	char s[64];

	for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
		if (rte_lcore_is_enabled(lcore_id) == 0)
			continue;

		if (numa_on)
			socketid = rte_lcore_to_socket_id(lcore_id);
		else
			socketid = 0;

		if (socketid >= NB_SOCKETS) {
			rte_exit(EXIT_FAILURE,
				 "Socket %d of lcore %u is out of range %d\n",
				 socketid, lcore_id, NB_SOCKETS);
		}
		if (direct_pools[socketid] == NULL) {
			snprintf(s, sizeof(s), "mbuf_pool_%d", socketid);
			direct_pools[socketid] = rte_pktmbuf_pool_create(s, nb_mbuf, MEMPOOL_CACHE_SIZE, 0,
									 RTE_MBUF_DEFAULT_BUF_SIZE, socketid);
			if (direct_pools[socketid] == NULL)
				rte_exit(EXIT_FAILURE, "Cannot init mbuf pool on socket %d\n", socketid);
			else
				XDPD_INFO("Allocated mbuf pool on socket %d\n", socketid);
		}
	}
	return 0;
}
#endif
static int init_mem(unsigned int socket_id, unsigned int nb_mbuf)
{
	char s[64];
	if (socket_id >= NB_SOCKETS) {
		rte_exit(EXIT_FAILURE,
					 "Socket %d is out of range %d\n",
					 socket_id, NB_SOCKETS);
	}
	if (direct_pools[socket_id] == NULL) {
		snprintf(s, sizeof(s), "mbuf_pool_%d", socket_id);
		XDPD_INFO(DRIVER_NAME" allocating rte_mempool for %u mbufs on socket %u\n", nb_mbuf, socket_id);
		if ((direct_pools[socket_id] = rte_pktmbuf_pool_create(s, nb_mbuf, MEMPOOL_CACHE_SIZE, 0,
								 RTE_MBUF_DEFAULT_BUF_SIZE, socket_id)) == NULL) {
			rte_exit(EXIT_FAILURE, "rte_mempool allocation failed on socket %u\n", socket_id);
		}
	}
	return ROFL_SUCCESS;
}

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

	//Make sure the link is down
	rte_eth_dev_set_link_down(ps->port_id);

	//Stop port
	rte_eth_dev_stop(ps->port_id);

	//Set pipeline state to UP
	if(likely(phy_port_mapping[ps->port_id]!=NULL)){
		phy_port_mapping[ps->port_id]->up = true;
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

/**
* Discovers logical cores.
*/
rofl_result_t iface_manager_discover_logical_cores(void){
#if 0
	//Initialize logical core structure: all lcores disabled
	for (int j = 0; j < RTE_MAX_LCORE; j++) {
		lcores[j].socket_id = -1;
		lcores[j].is_master = 0;
		lcores[j].is_enabled = 0;
		lcores[j].next_lcore_id = -1;
	}
	sockets.clear();
	cores.clear();

	//Get master lcore
	unsigned int master_lcore_id = rte_get_master_lcore();

	//Detect all lcores and their state
	for (unsigned int lcore_id = 0; lcore_id < rte_lcore_count(); lcore_id++) {
		if (lcore_id >= RTE_MAX_LCORE) {
			continue;
		}
		unsigned int socket_id = rte_lcore_to_socket_id(lcore_id);

		lcores[lcore_id].socket_id = socket_id;
		lcores[lcore_id].is_enabled = rte_lcore_is_enabled(lcore_id);

		//Get next lcore
		unsigned int next_lcore_id = RTE_MAX_LCORE;
		if ((next_lcore_id = rte_get_next_lcore(lcore_id, /*skip-master=*/1, /*wrap=*/1)) < RTE_MAX_LCORE) {
			lcores[lcore_id].next_lcore_id = next_lcore_id;
		}

		//master lcore?
		if (lcore_id == master_lcore_id) {
			lcores[lcore_id].is_master = 1;
		}

		//Store socket_id in sockets
		sockets.insert(socket_id);

		//Increase number of worker lcores for this socket
		if (lcore_id != master_lcore_id) {
			cores[socket_id].insert(lcore_id);
		}

		XDPD_INFO(DRIVER_NAME" adding lcore: %u %s on socket: %u, next lcore is: %u, #working lcores on this socket: %u\n",
				lcore_id,
				(lcores[lcore_id].is_master ? " as master" : ""),
				socket_id,
				lcores[lcore_id].next_lcore_id,
				cores[socket_id].size());
	}
#endif
	return ROFL_SUCCESS;
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
			YAML::Node& kni_args_node = it.second;

			if (not kni_name_node || not kni_name_node.IsScalar()) {
				continue;
			}
			strncpy(vport_names[port_name_index], kni_name_node.as<std::string>().c_str(), SWITCH_PORT_MAX_LEN_NAME);
			std::string ifname(vport_names[port_name_index]);

			/* assumption: ifname = "kni0", "kni1", ..., TODO: add check for "kniN" */
			std::string knidev_name("net_");
			knidev_name.append(ifname);

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
			YAML::Node& ring_args_node = it.second;

			if (not ring_name_node || not ring_name_node.IsScalar()) {
				continue;
			}
			strncpy(vport_names[port_name_index], ring_name_node.as<std::string>().c_str(), SWITCH_PORT_MAX_LEN_NAME);
			std::string ifname(vport_names[port_name_index]);

			/* assumption: ifname = "ring0", "ring1", ..., TODO: add check for "ringN" */
			std::string ringdev_name("net_");
			ringdev_name.append(ifname);

			std::string ringdev_args;
			if (ring_args_node && ring_args_node.IsScalar()) {
				ringdev_args = ring_args_node.as<std::string>();
			}

			XDPD_INFO(DRIVER_NAME"[ifaces] adding virtual PMD ring port: %s with args: %s\n", ringdev_name.c_str(), ringdev_args.c_str());

			/* initialize ring pmd device */
			if ((ret = rte_vdev_init(ringdev_name.c_str(), ringdev_args.c_str())) < 0) {
				switch (ret) {
				case -EINVAL: {
					XDPD_ERR(DRIVER_NAME"[ifaces] initialization of ring dev %s with args \"%s\" failed (EINVAL)\n",
							ifname.c_str(), ringdev_args.c_str());
				} break;
				case -EEXIST: {
					XDPD_ERR(DRIVER_NAME"[ifaces] initialization of ring dev %s with args \"%s\" failed (EEXIST)\n",
							ifname.c_str(), ringdev_args.c_str());
				} break;
				case -ENOMEM: {
					XDPD_ERR(DRIVER_NAME"[ifaces] initialization of ring dev %s with args \"%s\" failed (ENOMEM)\n",
							ifname.c_str(), ringdev_args.c_str());
				} break;
				default: {
					XDPD_ERR(DRIVER_NAME"[ifaces] initialization of ring dev %s with args \"%s\" failed\n",
							ifname.c_str(), ringdev_args.c_str());
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
	size_t nb_mbuf[RTE_MAX_NUMA_NODES]; //The required space per NUMA node
	YAML::Node node;
	int ret = 0;

	for (unsigned int socket_id = 0; socket_id < RTE_MAX_NUMA_NODES; ++socket_id) {
		nb_mbuf[socket_id] = rte_eth_dev_count() * rx_lcores.size() * IO_IFACE_MAX_PKT_BURST + rx_lcores.size() * MEMPOOL_CACHE_SIZE;
	}

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
		unsigned int nb_rx_queues = rx_lcores[socket_id].size() < dev_info.max_rx_queues ? rx_lcores[socket_id].size() : dev_info.max_rx_queues;

		nb_mbuf[socket_id] += /*rx*/nb_rx_queues * dev_info.rx_desc_lim.nb_max + /*tx*/nb_rx_queues * dev_info.tx_desc_lim.nb_max;
	}

	//Initialize rte_mempool for all active NUMA nodes
	for (auto socket_id : sockets) {
		init_mem(socket_id, nb_mbuf[socket_id]);
	}

	//Iterate over all available physical ports
	for (uint16_t port_id = 0; port_id < rte_eth_dev_count(); port_id++) {

		rte_eth_dev_info_get(port_id, &dev_info);
		memset(s_pci_addr, 0, sizeof(s_pci_addr));
		if (dev_info.pci_dev) {
			rte_pci_device_name(&(dev_info.pci_dev->addr), s_pci_addr, sizeof(s_pci_addr));
		}

		if (port_id >= RTE_MAX_ETHPORTS) {
			return ROFL_FAILURE;
		}

		unsigned int socket_id = rte_eth_dev_socket_id(port_id);


		/* virtual ports appear as physical ones here (including kni, ring, ...)
		 * However, they are bound to NUMA node LCORE_ID_ANY. We bind all those
		 * virtual devices to the NUMA socket the master lcore is running on.
		 * Thus, all virtual devices use the same NUMA node. It may be necessary to
		 * change this static mapping in the future to avoid high load on the
		 * master lcore NUMA node. */

		/* for ports bound to LCORE_ID_ANY (virtual interfaces, e.g., kni), use socket_id of master lcore */
		if (dev_info.driver_name == std::string("net_kni")) {
			socket_id = rte_lcore_to_socket_id(rte_get_master_lcore());
			XDPD_DEBUG(DRIVER_NAME"[ifaces] physical port: %u, mapping LCORE_ID_ANY to socket %u used by master lcore\n", port_id, socket_id);
			phyports[port_id].is_virtual = true;
		} else
		if (dev_info.driver_name == std::string("net_ring")) {
			phyports[port_id].is_virtual = true;
		}

		phyports[port_id].socket_id = socket_id;
		phyports[port_id].is_enabled = 0;

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

		// port disabled in configuration file?
		if (not phyports[port_id].is_virtual && not iface_manager_get_port_setting_as<bool>(s_pci_addr, "enabled")) {
			XDPD_INFO(DRIVER_NAME"[ifaces] skipping physical port: %u (port explicitly \"disabled\") on socket: %u, driver: %s, firmware: %s, PCI address: %s\n",
					port_id, socket_id, dev_info.driver_name, s_fw_version, s_pci_addr);
			continue;
		}

		phyports[port_id].is_enabled = 1;

		// is port a virtual function and has a parent device?
		if (not phyports[port_id].is_virtual && iface_manager_port_setting_exists(s_pci_addr, "parent")) {
			phyports[port_id].is_vf = 1;
			phyports[port_id].parent_port_id = iface_manager_pci_address_to_port_id(iface_manager_get_port_setting_as<std::string>(s_pci_addr, "parent"));
			phyports[port_id].vf_id = phyports[phyports[port_id].parent_port_id].nb_vfs++;
			if (phyports[port_id].parent_port_id == port_id) {
				XDPD_ERR(DRIVER_NAME"[ifaces] unlikely configuration detected: parent port_id == port_id (%u), probably a misconfiguration?\n", port_id);
			}
		}

		//number of configured RX queues on device should not exceed number of worker lcores on socket
		unsigned int nb_rx_queues = rx_lcores[socket_id].size() < dev_info.max_rx_queues ? rx_lcores[socket_id].size() : dev_info.max_rx_queues;

		//number of configured TX queues on device should not exceed number of worker lcores on socket
		unsigned int nb_tx_lcores = 0;
		for (auto it : tx_lcores) {
			nb_tx_lcores += it.second.size();
		}
		unsigned int nb_tx_queues = nb_tx_lcores < dev_info.max_tx_queues ? nb_tx_lcores : dev_info.max_tx_queues;


		phyports[port_id].nb_rx_queues = nb_rx_queues;
		phyports[port_id].nb_tx_queues = nb_tx_queues;

		if (phyports[port_id].nb_rx_queues == 0) {
			XDPD_INFO(DRIVER_NAME"[ifaces] skipping physical port: %u on socket: %u with nb_rx_queues: %u\n",
					port_id, socket_id, nb_rx_queues);
			continue;
		}

		if (phyports[port_id].nb_tx_queues == 0) {
			XDPD_INFO(DRIVER_NAME"[ifaces] skipping physical port: %u on socket: %u with nb_tx_queues: %u\n",
					port_id, socket_id, nb_tx_queues);
			continue;
		}


		XDPD_INFO(DRIVER_NAME"[ifaces] adding physical port: %u on socket: %u with max_rx_queues: %u, rx_queues in use: %u, max_tx_queues: %u, tx_queues in use: %u, driver: %s, firmware: %s, PCI address: %s\n",
				port_id, socket_id, dev_info.max_rx_queues, nb_rx_queues, dev_info.max_tx_queues, nb_tx_queues, dev_info.driver_name, s_fw_version, s_pci_addr);


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
			rx_core_tasks[lcore_id].rx_queues[index].port_id = port_id;
			rx_core_tasks[lcore_id].rx_queues[index].queue_id = rx_queue_id;
			rx_core_tasks[lcore_id].nb_rx_queues++;
			XDPD_INFO(DRIVER_NAME"[ifaces] assigning physical port: %u, rxqueue: %u on socket: %u to lcore: %u on socket: %u, nb_rx_queues: %u\n",
					port_id, rx_queue_id, socket_id, lcore_id, rte_lcore_to_socket_id(lcore_id), rx_core_tasks[lcore_id].nb_rx_queues);
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
			rgname << "task-" << lcore_id;
			rgname << "port-" << port_id;

			/* store txring-drain-max-queuesize parameter for this port */
			if (not phyports[port_id].is_virtual && iface_manager_port_setting_exists(s_pci_addr, "txring-drain-max-queue-size")) {
				tx_core_tasks[lcore_id].txring_drain_max_queue_size[port_id] = (unsigned int)ceil(log2(iface_manager_get_port_setting_as<unsigned int>(s_pci_addr, "txring-drain-max-queue-size")));
			} else {
				tx_core_tasks[lcore_id].txring_drain_max_queue_size[port_id] = (unsigned int)ceil(log2(PROCESSING_TXRING_DRAIN_MAX_QUEUE_SIZE_DEFAULT));
			}

			/* store txring-drain-interval parameter for this port */
			if (not phyports[port_id].is_virtual && iface_manager_port_setting_exists(s_pci_addr, "txring-drain-interval")) {
				tx_core_tasks[lcore_id].txring_drain_interval[port_id] = iface_manager_get_port_setting_as<uint64_t>(s_pci_addr, "txring-drain-interval");
			} else {
				tx_core_tasks[lcore_id].txring_drain_interval[port_id] = PROCESSING_TXRING_DRAIN_INTERVAL_DEFAULT;
			}

			/* store txring-drain-threshold parameter for this port */
			if (not phyports[port_id].is_virtual && iface_manager_port_setting_exists(s_pci_addr, "txring-drain-threshold")) {
				tx_core_tasks[lcore_id].txring_drain_threshold[port_id] = iface_manager_get_port_setting_as<unsigned int>(s_pci_addr, "txring-drain-threshold");
			} else {
				tx_core_tasks[lcore_id].txring_drain_threshold[port_id] = PROCESSING_TXRING_DRAIN_THRESHOLD_DEFAULT;
			}

			/* create RTE ring for queuing packets between workers and tx threads */
			if (rte_ring_create(rgname.str().c_str(), tx_core_tasks[lcore_id].txring_drain_max_queue_size[port_id], socket_id, RING_F_SP_ENQ | RING_F_SC_DEQ) == NULL) {
				XDPD_DEBUG(DRIVER_NAME"[ifaces] unable to create tx-ring: %s for port-id: %u\n", rgname.str().c_str(), port_id);
				return ROFL_FAILURE;
			}


			/*
			 * store txqueue on eth-dev for this port and TX task
			 */

			tx_core_tasks[lcore_id].tx_queues[port_id].enabled = 1;
			tx_core_tasks[lcore_id].tx_queues[port_id].queue_id = tx_queue_id;
			tx_core_tasks[lcore_id].nb_tx_queues++;
			XDPD_INFO(DRIVER_NAME"[ifaces] assigning physical port: %u, txqueue: %u on socket: %u to lcore: %u on socket: %u, nb_tx_queues: %u\n",
					port_id, tx_queue_id, socket_id, lcore_id, rte_lcore_to_socket_id(lcore_id), tx_core_tasks[lcore_id].nb_tx_queues);
			if (tx_queue_id >= (phyports[port_id].nb_tx_queues - 1)) {
				break;
			}
			tx_queue_id = (tx_queue_id < (phyports[port_id].nb_tx_queues - 1)) ? tx_queue_id + 1 : 0;
		}



		//Configure the port
		struct rte_eth_conf eth_conf;
		memset(&eth_conf, 0, sizeof(eth_conf));

		//receive side
		eth_conf.link_speeds = ETH_LINK_SPEED_AUTONEG; //auto negotiation enabled
		eth_conf.lpbk_mode = 0; //loopback disabled
		eth_conf.rxmode.mq_mode = ETH_MQ_RX_RSS; //enable Receive Side Scaling (RSS) only
		eth_conf.rxmode.offloads = dev_info.rx_offload_capa;
		eth_conf.rxmode.max_rx_pkt_len = ETHER_MAX_LEN;
		eth_conf.rxmode.header_split = 0;
		eth_conf.rxmode.hw_ip_checksum = 1;
		eth_conf.rxmode.hw_vlan_extend = 0;
		eth_conf.rxmode.hw_vlan_filter = 1;
		eth_conf.rxmode.hw_vlan_strip = 1;
		eth_conf.rxmode.hw_strip_crc = 1;
		eth_conf.rxmode.jumbo_frame = 0;
		eth_conf.rxmode.enable_scatter = 0;
		eth_conf.rxmode.enable_lro = 0;
		eth_conf.rxmode.split_hdr_size = 0;
		eth_conf.rxmode.max_rx_pkt_len = IO_MAX_PACKET_SIZE;
		eth_conf.rx_adv_conf.rss_conf.rss_key = NULL;
		eth_conf.rx_adv_conf.rss_conf.rss_key_len = 0;
		eth_conf.rx_adv_conf.rss_conf.rss_hf = /*ETH_RSS_L2_PAYLOAD |*/ ETH_RSS_IP | ETH_RSS_TCP | ETH_RSS_UDP;

		//transmit side
		eth_conf.txmode.mq_mode = ETH_MQ_TX_NONE;
		eth_conf.txmode.offloads = dev_info.tx_offload_capa;

		//configure port
		if ((ret = rte_eth_dev_configure(port_id, nb_rx_queues, nb_tx_queues, &eth_conf)) < 0) {
			switch (ret) {
			case -EINVAL: {
				XDPD_ERR(DRIVER_NAME"[ifaces] failed to configure port %u: rte_eth_dev_configure() (EINVAL)\n", port_id);
			} break;
			case -ENOTSUP: {
				XDPD_ERR(DRIVER_NAME"[ifaces] failed to configure port %u: rte_eth_dev_configure() (ENOTSUP)\n", port_id);
			} break;
			case -EBUSY: {
				XDPD_ERR(DRIVER_NAME"[ifaces] failed to configure port %u: rte_eth_dev_configure() (EBUSY)\n", port_id);
			} break;
			default: {
				XDPD_ERR(DRIVER_NAME"[ifaces] failed to configure port %u: rte_eth_dev_configure()\n", port_id);
			};
			}
			XDPD_ERR(DRIVER_NAME"[ifaces] failed to configure port: %u, aborting\n", port_id);
			return ROFL_FAILURE;
		}

		// configure transmit queues
		for (uint16_t tx_queue_id = 0; tx_queue_id < /*no typo!*/nb_tx_queues; tx_queue_id++) {
			uint16_t nb_tx_desc = 0;
			struct rte_eth_txconf eth_txconf;

			if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_I40E_PF, sizeof(DPDK_DRIVER_NAME_I40E_PF)) == 0){

				// values for i40e PF
				nb_tx_desc = dev_info.tx_desc_lim.nb_max;
				eth_txconf.tx_thresh.pthresh = I40E_DEFAULT_TX_PTHRESH;
				eth_txconf.tx_thresh.hthresh = I40E_DEFAULT_TX_HTHRESH;
				eth_txconf.tx_thresh.wthresh = I40E_DEFAULT_TX_WTHRESH;
				eth_txconf.tx_free_thresh = I40E_DEFAULT_TX_FREE_THRESH; //use default, e.g., I40E_DEFAULT_TX_FREE_THRESH = 32
				eth_txconf.tx_rs_thresh = I40E_DEFAULT_TX_RSBIT_THRESH; //use default, e.g., I40E_DEFAULT_TX_RSBIT_THRESH = 32
				eth_txconf.tx_deferred_start = 0;
				eth_txconf.txq_flags = ETH_TXQ_FLAGS_IGNORE;
				//eth_txconf.txq_flags = ETH_TXQ_FLAGS_NOMULTSEGS;
				eth_txconf.offloads = dev_info.tx_queue_offload_capa;

			} else
			if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_I40E_VF, sizeof(DPDK_DRIVER_NAME_I40E_VF)) == 0){

				// are these values also valid for i40e VF?
				nb_tx_desc = dev_info.tx_desc_lim.nb_max;
				eth_txconf.tx_thresh.pthresh = I40E_DEFAULT_TX_PTHRESH;
				eth_txconf.tx_thresh.hthresh = I40E_DEFAULT_TX_HTHRESH;
				eth_txconf.tx_thresh.wthresh = I40E_DEFAULT_TX_WTHRESH;
				eth_txconf.tx_free_thresh = I40E_DEFAULT_TX_FREE_THRESH; //use default, e.g., I40E_DEFAULT_TX_FREE_THRESH = 32
				eth_txconf.tx_rs_thresh = I40E_DEFAULT_TX_RSBIT_THRESH; //use default, e.g., I40E_DEFAULT_TX_RSBIT_THRESH = 32
				eth_txconf.tx_deferred_start = 0;
				eth_txconf.txq_flags = ETH_TXQ_FLAGS_IGNORE;
				//eth_txconf.txq_flags = ETH_TXQ_FLAGS_NOMULTSEGS;
				eth_txconf.offloads = dev_info.tx_queue_offload_capa;

			} else if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_IXGBE_PF, sizeof(DPDK_DRIVER_NAME_IXGBE_PF)) == 0) {

				nb_tx_desc = dev_info.tx_desc_lim.nb_max;
				eth_txconf.tx_thresh.pthresh = IXGBE_DEFAULT_TX_PTHRESH;
				eth_txconf.tx_thresh.hthresh = IXGBE_DEFAULT_TX_HTHRESH;
				eth_txconf.tx_thresh.wthresh = IXGBE_DEFAULT_TX_WTHRESH;
				eth_txconf.tx_free_thresh = IXGBE_DEFAULT_TX_FREE_THRESH; //use default, e.g., IXGBE_DEFAULT_TX_FREE_THRESH = 32
				eth_txconf.tx_rs_thresh = IXGBE_DEFAULT_TX_RSBIT_THRESH; //use default, e.g., IXGBE_DEFAULT_TX_RSBIT_THRESH = 32
				eth_txconf.tx_deferred_start = 0;
				eth_txconf.txq_flags = ETH_TXQ_FLAGS_IGNORE;
				//eth_txconf.txq_flags = ETH_TXQ_FLAGS_NOMULTSEGS;
				eth_txconf.offloads = dev_info.tx_queue_offload_capa;

			} else {

				//defaults for unknown driver
				nb_tx_desc = dev_info.tx_desc_lim.nb_max;
				eth_txconf.tx_thresh.pthresh = TX_PTHRESH;
				eth_txconf.tx_thresh.hthresh = TX_HTHRESH;
				eth_txconf.tx_thresh.wthresh = TX_WTHRESH;
				eth_txconf.tx_free_thresh = 0; //use default, e.g., I40E_DEFAULT_TX_FREE_THRESH = 32
				eth_txconf.tx_rs_thresh = 0; //use default, e.g., I40E_DEFAULT_TX_RSBIT_THRESH = 32
				eth_txconf.tx_deferred_start = 0;
				eth_txconf.txq_flags = ETH_TXQ_FLAGS_IGNORE;
				//eth_txconf.txq_flags = ETH_TXQ_FLAGS_NOMULTSEGS;
				eth_txconf.offloads = dev_info.tx_queue_offload_capa;

			}

			//configure txqueue
			if (rte_eth_tx_queue_setup(port_id, tx_queue_id, nb_tx_desc, socket_id, &eth_txconf) < 0) {
				XDPD_ERR(DRIVER_NAME" Failed to configure port: %u tx-queue: %u, aborting\n", port_id, tx_queue_id);
				return ROFL_FAILURE;
			}
		}


		// configure receive queues
		for (uint16_t rx_queue_id = 0; rx_queue_id < nb_rx_queues; rx_queue_id++) {
			uint16_t nb_rx_desc = 0;
			struct rte_eth_rxconf eth_rxconf;

			if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_I40E_PF, sizeof(DPDK_DRIVER_NAME_I40E_PF)) == 0){

				// values for i40e PF
				nb_rx_desc = dev_info.rx_desc_lim.nb_max;
				eth_rxconf.rx_thresh.pthresh = I40E_DEFAULT_RX_PTHRESH;
				eth_rxconf.rx_thresh.hthresh = I40E_DEFAULT_RX_HTHRESH;
				eth_rxconf.rx_thresh.wthresh = I40E_DEFAULT_RX_WTHRESH;
				eth_rxconf.rx_drop_en = 1; //drop packets when descriptor space is exhausted
				eth_rxconf.rx_free_thresh = I40E_DEFAULT_RX_FREE_THRESH;
				eth_rxconf.rx_deferred_start = 0;
				eth_rxconf.offloads = dev_info.rx_queue_offload_capa;

			} else
			if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_I40E_VF, sizeof(DPDK_DRIVER_NAME_I40E_VF)) == 0){

				// are these values also valid for i40e VF?
				nb_rx_desc = dev_info.rx_desc_lim.nb_max;
				eth_rxconf.rx_thresh.pthresh = I40E_DEFAULT_RX_PTHRESH;
				eth_rxconf.rx_thresh.hthresh = I40E_DEFAULT_RX_HTHRESH;
				eth_rxconf.rx_thresh.wthresh = I40E_DEFAULT_RX_WTHRESH;
				eth_rxconf.rx_drop_en = 1; //drop packets when descriptor space is exhausted
				eth_rxconf.rx_free_thresh = I40E_DEFAULT_RX_FREE_THRESH;
				eth_rxconf.rx_deferred_start = 0;
				eth_rxconf.offloads = dev_info.rx_queue_offload_capa;

			} else if(strncmp(dev_info.driver_name, DPDK_DRIVER_NAME_IXGBE_PF, sizeof(DPDK_DRIVER_NAME_IXGBE_PF)) == 0) {

				nb_rx_desc = dev_info.rx_desc_lim.nb_max;
				eth_rxconf.rx_thresh.pthresh = IXGBE_DEFAULT_RX_PTHRESH;
				eth_rxconf.rx_thresh.hthresh = IXGBE_DEFAULT_RX_HTHRESH;
				eth_rxconf.rx_thresh.wthresh = IXGBE_DEFAULT_RX_WTHRESH;
				eth_rxconf.rx_drop_en = 1; //drop packets when descriptor space is exhausted
				eth_rxconf.rx_free_thresh = IXGBE_DEFAULT_RX_FREE_THRESH;
				eth_rxconf.rx_deferred_start = 0;
				eth_rxconf.offloads = dev_info.rx_queue_offload_capa;

			} else {

				//defaults for unknown driver
				nb_rx_desc = dev_info.rx_desc_lim.nb_max;
				eth_rxconf.rx_thresh.pthresh = RX_PTHRESH;
				eth_rxconf.rx_thresh.hthresh = RX_HTHRESH;
				eth_rxconf.rx_thresh.wthresh = RX_WTHRESH;
				eth_rxconf.rx_free_thresh = 0;
				eth_rxconf.rx_deferred_start = 0;
				eth_rxconf.offloads = dev_info.rx_queue_offload_capa;

			}

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

	unsigned int vport_name_index = 0;
	//Iterate over all available physical ports
	for (uint16_t port_id = 0; port_id < rte_eth_dev_count(); port_id++) {
		char port_name[SWITCH_PORT_MAX_LEN_NAME];
		switch_port_t* port;
		int socket_id;

		if (not phyports[port_id].is_enabled) {
			continue;
		}

		rte_eth_dev_info_get(port_id, &dev_info);

		/* net_kni PMD */
		if (dev_info.driver_name == std::string("net_kni")) {
			snprintf (port_name, SWITCH_PORT_MAX_LEN_NAME, vport_names[vport_name_index++]);
			socket_id = rte_lcore_to_socket_id(rte_get_master_lcore());
		} else
		/* net_ring PMD */
		if (dev_info.driver_name == std::string("net_ring")) {
			snprintf (port_name, SWITCH_PORT_MAX_LEN_NAME, vport_names[vport_name_index++]);
			socket_id = rte_eth_dev_socket_id(port_id);
		} else
		/* physical ports */
		if (true) {
			memset(s_pci_addr, 0, sizeof(s_pci_addr));
			if (dev_info.pci_dev) {
				rte_pci_device_name(&(dev_info.pci_dev->addr), s_pci_addr, sizeof(s_pci_addr));
			}
			snprintf (port_name, SWITCH_PORT_MAX_LEN_NAME, iface_manager_get_port_setting_as<std::string>(s_pci_addr, "ifname").c_str());
			socket_id = rte_eth_dev_socket_id(port_id);
		}

		XDPD_INFO(DRIVER_NAME" adding xdpd port: %s for dpdk port: %u on socket: %u\n", port_name, port_id, socket_id);

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

	//Names are composed following vlinkX-Y
	//Where X is the virtual link number (0... N-1)
	//Y is the edge 0 (left) 1 (right) of the connectio
	static unsigned int num_of_vlinks=0;
	char port_name[PORT_QUEUE_MAX_LEN_NAME];
	char queue_name[PORT_QUEUE_MAX_LEN_NAME];
	uint64_t port_capabilities=0x0;
	uint16_t randnum = 0;
	unsigned int i;

	//Init the pipeline ports
	snprintf(port_name,PORT_QUEUE_MAX_LEN_NAME, "vlink%u_%u", num_of_vlinks, 0);

	*vport1 = switch_port_init(port_name, true, PORT_TYPE_VIRTUAL, PORT_STATE_NONE);
	snprintf(port_name,PORT_QUEUE_MAX_LEN_NAME, "vlink%u_%u", num_of_vlinks, 1);

	*vport2 = switch_port_init(port_name, true, PORT_TYPE_VIRTUAL, PORT_STATE_NONE);
	
	if(*vport1 == NULL || *vport2 == NULL){
		XDPD_ERR(DRIVER_NAME"[iface_manager] Unable to allocate memory for virtual ports\n");
		assert(0);
		goto PORT_MANAGER_CREATE_VLINK_PAIR_ERROR;
	}

	//Initalize port features(Marking as 1G)
	port_capabilities |= PORT_FEATURE_1GB_FD;
	switch_port_add_capabilities(&(*vport1)->curr, (port_features_t)port_capabilities);	
	switch_port_add_capabilities(&(*vport1)->advertised, (port_features_t)port_capabilities);	
	switch_port_add_capabilities(&(*vport1)->supported, (port_features_t)port_capabilities);	
	switch_port_add_capabilities(&(*vport1)->peer, (port_features_t)port_capabilities);	

	randnum = (uint16_t)rand();
	(*vport1)->hwaddr[0] = ((uint8_t*)&randnum)[0];
	(*vport1)->hwaddr[1] = ((uint8_t*)&randnum)[1];
	randnum = (uint16_t)rand();
	(*vport1)->hwaddr[2] = ((uint8_t*)&randnum)[0];
	(*vport1)->hwaddr[3] = ((uint8_t*)&randnum)[1];
	randnum = (uint16_t)rand();
	(*vport1)->hwaddr[4] = ((uint8_t*)&randnum)[0];
	(*vport1)->hwaddr[5] = ((uint8_t*)&randnum)[1];

	// locally administered MAC address
	(*vport1)->hwaddr[0] &= ~(1 << 0);
	(*vport1)->hwaddr[0] |=  (1 << 1);

	//Add queues
	for(i=0;i<IO_IFACE_NUM_QUEUES;i++){
		snprintf(queue_name, PORT_QUEUE_MAX_LEN_NAME, "%s%d", "queue", i);
		if(switch_port_add_queue((*vport1), i, (char*)&queue_name, IO_IFACE_MAX_PKT_BURST, 0, 0) != ROFL_SUCCESS){
			XDPD_ERR(DRIVER_NAME"[iface_manager] Cannot configure queues on device (pipeline): %s\n", (*vport1)->name);
			assert(0);
			goto PORT_MANAGER_CREATE_VLINK_PAIR_ERROR;
		}
	}

	switch_port_add_capabilities(&(*vport2)->curr, (port_features_t)port_capabilities);	
	switch_port_add_capabilities(&(*vport2)->advertised, (port_features_t)port_capabilities);	
	switch_port_add_capabilities(&(*vport2)->supported, (port_features_t)port_capabilities);	
	switch_port_add_capabilities(&(*vport2)->peer, (port_features_t)port_capabilities);	

	randnum = (uint16_t)rand();
	(*vport2)->hwaddr[0] = ((uint8_t*)&randnum)[0];
	(*vport2)->hwaddr[1] = ((uint8_t*)&randnum)[1];
	randnum = (uint16_t)rand();
	(*vport2)->hwaddr[2] = ((uint8_t*)&randnum)[0];
	(*vport2)->hwaddr[3] = ((uint8_t*)&randnum)[1];
	randnum = (uint16_t)rand();
	(*vport2)->hwaddr[4] = ((uint8_t*)&randnum)[0];
	(*vport2)->hwaddr[5] = ((uint8_t*)&randnum)[1];
	
	// locally administered MAC address
	(*vport2)->hwaddr[0] &= ~(1 << 0);
	(*vport2)->hwaddr[0] |=  (1 << 1);

	//Add queues
	for(i=0;i<IO_IFACE_NUM_QUEUES;i++){
		snprintf(queue_name, PORT_QUEUE_MAX_LEN_NAME, "%s%d", "queue", i);
		if(switch_port_add_queue((*vport2), i, (char*)&queue_name, IO_IFACE_MAX_PKT_BURST, 0, 0) != ROFL_SUCCESS){
			XDPD_ERR(DRIVER_NAME"[iface_manager] Cannot configure queues on device (pipeline): %s\n", (*vport2)->name);
			assert(0);
			goto PORT_MANAGER_CREATE_VLINK_PAIR_ERROR;
		}
	}

	//Interlace them
	(*vport2)->platform_port_state = *vport1;	
	(*vport1)->platform_port_state = *vport2;	


	//Add them to the physical switch
	if( physical_switch_add_port(*vport1) != ROFL_SUCCESS ){
		XDPD_ERR(DRIVER_NAME"[iface_manager] Unable to allocate memory for virtual ports\n");
		assert(0);
		goto PORT_MANAGER_CREATE_VLINK_PAIR_ERROR;	

	}
	if( physical_switch_add_port(*vport2) != ROFL_SUCCESS ){
		XDPD_ERR(DRIVER_NAME"[iface_manager] Unable to allocate memory for virtual ports\n");
		assert(0);
		goto PORT_MANAGER_CREATE_VLINK_PAIR_ERROR;	

	}

	//Increment counter and return
	num_of_vlinks++; 

	return ROFL_SUCCESS;

PORT_MANAGER_CREATE_VLINK_PAIR_ERROR:
	if(*vport1)
		switch_port_destroy(*vport1);
	if(*vport2)
		switch_port_destroy(*vport2);
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
		switch_port_t* port_pair = (switch_port_t*)port->platform_port_state;
		//Set link flag on both ports
		if(port_pair->up){
			port->state &= ~PORT_STATE_LINK_DOWN;
			port_pair->state &= ~PORT_STATE_LINK_DOWN;
		}else{
			port->state |= PORT_STATE_LINK_DOWN;
			port_pair->state |= PORT_STATE_LINK_DOWN;
		}
	}
	else if(port->type == PORT_TYPE_NF_SHMEM)
	{
		/*
		*  DPDK SECONDARY NF
		*/
		if(!port->up)
		{
			//Was down
			if(nf_iface_manager_bring_up_port(port) != ROFL_SUCCESS)
			{
				XDPD_ERR(DRIVER_NAME"[port_manager] Cannot start DPDK SECONDARY NF port: %s\n",port->name);
				assert(0);
				return ROFL_FAILURE; 
			}
		}
	}else if(port->type == PORT_TYPE_NF_EXTERNAL)
	{
		/*
		*	DPDK KNI NF
		*/
		if(!port->up)
		{
			//Was down
			if(nf_iface_manager_bring_up_port(port) != ROFL_SUCCESS)
			{
				XDPD_ERR(DRIVER_NAME"[port_manager] Cannot start DPDK KNI NF port: %s\n",port->name);
				assert(0);
				return ROFL_FAILURE; 
			}
		}
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
		switch_port_t* port_pair = (switch_port_t*)port->platform_port_state;
		port->up = false;

		//Set links as down	
		port->state |= PORT_STATE_LINK_DOWN;
		port_pair->state |= PORT_STATE_LINK_DOWN;
	}
	else if(port->type == PORT_TYPE_NF_SHMEM) {
		/*
		* NF port
		*/
		if(port->up) {
			if(nf_iface_manager_bring_down_port(port) != ROFL_SUCCESS) {
				XDPD_ERR(DRIVER_NAME"[port_manager] Cannot stop DPDK SECONDARY NF port: %s\n",port->name);
				assert(0);
				return ROFL_FAILURE; 
			}
		}		
		port->up = false;
	}else if(port->type == PORT_TYPE_NF_EXTERNAL) {
		/*
		*	KNI NF
		*/
		if(port->up){
			if(nf_iface_manager_bring_down_port(port) != ROFL_SUCCESS) {
				XDPD_ERR(DRIVER_NAME"[port_manager] Cannot stop DPDK KNI NF port: %s\n",port->name);
				assert(0);
				return ROFL_FAILURE; 
			}
		}
		port->up = false;
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

