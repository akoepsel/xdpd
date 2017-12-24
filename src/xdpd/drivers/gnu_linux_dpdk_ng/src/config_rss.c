#include "config_rss.h"

//The set of available logical cores (lcore) per NUMA node (=socket)
struct lcore lcores[RTE_MAX_LCORE];

//The set of available physical ports (phyport) per NUMA node (=socket)
struct phyport phyports[RTE_MAX_ETHPORTS];

//virtual port names (kni0, kni1, ring0, ring1)
char vport_names[RTE_MAX_ETHPORTS][SWITCH_PORT_MAX_LEN_NAME];
