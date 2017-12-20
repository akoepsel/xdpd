#include "config_rss.h"

//The set of available logical cores (lcore) per NUMA node (=socket)
struct lcore lcores[RTE_MAX_LCORE];

//The set of available physical ports (phyport) per NUMA node (=socket)
struct phyport phyports[RTE_MAX_ETHPORTS];

char vport_names[RTE_MAX_ETHPORTS][SWITCH_PORT_MAX_LEN_NAME];

//Set default values TODO remove
// default values wrt. 2 LSI with 2 physical ports and 2 kni ports
struct lcore_params lcore_params[LCORE_PARAMS_MAX] = {
	// physical
	{0, 0, 0,  2}, {0, 0, 1,  3}, {0, 0, 2,  4}, {0, 0, 3,  5},
	{0, 1, 0,  5}, {0, 1, 1,  6}, {0, 1, 2,  8}, {0, 1, 3,  9},
	{1, 2, 0, 10}, {1, 2, 1, 11}, {1, 2, 2, 12}, {1, 2, 3, 13},
	{1, 3, 0, 14}, {1, 3, 1, 15}, {1, 3, 2, 16}, {1, 3, 3, 17},

	// kni
	{0, 4, 0, 18},
	{0, 5, 0, 18},
	{1, 6, 0, 18},
	{1, 7, 0, 18},
};
uint16_t nb_lcore_params = 20;
