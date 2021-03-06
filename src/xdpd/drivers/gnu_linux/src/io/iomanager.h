/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef IOMANAGER_H
#define IOMANAGER_H 1

#include <pthread.h>
#include <map>
#include <rofl_datapath.h>
#include <rofl/datapath/pipeline/switch_port.h>
#include <semaphore.h>
#include "../config.h"
#include "scheduler/ioscheduler.h"
#include "ports/ioport.h"
#include "../util/safevector.h" 
#include "../util/compiler_assert.h" 

/**
* @file iomanager.h
* @author Marc Sune<marc.sune (at) bisdn.de>
*
* @brief This static class is in charge of of launch/stopping of the I/O threads, 
* dealing with data packet tx/rx.
*
* TODO: in depth explanation
* 
*/

namespace xdpd {
namespace gnu_linux {

#define DEFAULT_MAX_THREADS_PER_PG 5
//WARNING: you don't want to change this, unless you really(really) know what you are doing
#define DEFAULT_THREADS_PER_PG 1
COMPILER_ASSERT( INVALID_default_threads_per_pg , (DEFAULT_THREADS_PER_PG == 1) );


/**
* Port group type
*
* @ingroup driver_gnu_linux_io
*/
typedef enum pg_type{
	PG_RX,
	PG_TX,
}pg_type_t;

/**
* @brief Portgroup thread state
*
* @ingroup driver_gnu_linux_io
*/
class portgroup_state {

public:
	//Group id
	unsigned int id;	

	//Keep on flag
	bool keep_on;

	//Threading information
	unsigned int num_of_threads;
	pthread_t thread_state[DEFAULT_MAX_THREADS_PER_PG];

	//State synchronization condition (iomanager->RX/TX threads)
	sem_t sync_sem;

	//Port group type (RX or TX)
	pg_type_t type;
	
	// I/O port information
	safevector<ioport*>* ports; 		//All ports in the group
	safevector<ioport*>* running_ports;	//Ports of the group currently performing I/O operations
	uint32_t running_hash;			//Pseudo-hash over the running_ports state
	
};

/**
* @brief I/O manager, creates and destroys (launches and stops) I/O threads to work on the ports, or specifically a set of ports (portgroups). 
* This class is purely static.
*
* @ingroup driver_gnu_linux_io
*/
class iomanager{ 

public:
	/* Methods */
	//Group mgmt
	static rofl_result_t init( unsigned int _rx_groups = IO_RX_THREADS, unsigned int _tx_groups = IO_TX_THREADS);
	static rofl_result_t destroy( void ){ return delete_all_groups(); };

	/*
	* Port management (external interface)
	*/	
	static rofl_result_t add_port(ioport* port);
	static rofl_result_t remove_port(ioport* port);

	/*
	* Port control
	*/
	static rofl_result_t bring_port_up(ioport* port);
	static rofl_result_t bring_port_down(ioport* port, bool mutex_locked=false);
	
	/*
	* Checkpoint for I/O threads to keep on working. Called by schedulers
	*/ 
	inline static bool keep_on_working(portgroup_state* pg){ return pg->keep_on;};

	/*
	* Signal that PG state has been syncrhonized within a particular I/O thread 
	*/
	inline static void signal_as_synchronized(portgroup_state* pg){ sem_post(&pg->sync_sem); };

	/* Utils */ 
	static void dump_state(bool mutex_locked);
	static portgroup_state* get_group(int grp_id){
		return portgroups[grp_id];
	};
	static int get_group_id_by_port(ioport* port, pg_type_t type);
protected:

	//Constants
	static const unsigned int DEFAULT_THREADS_PER_PORTGROUP = DEFAULT_THREADS_PER_PG;
	static const unsigned int MAX_THREADS_PER_PORTGROUP = DEFAULT_MAX_THREADS_PER_PG;

	//Totla number of groupsNumber of port_groups created
	static unsigned int num_of_groups;
	//Total number of RX groups
	static unsigned int num_of_rx_groups;
	//Total number of TX groups
	static unsigned int num_of_tx_groups;

	/*
	* Scheduling state 
	*/
	static unsigned int next_rx_sched_grp_id;
	static unsigned int next_tx_sched_grp_id;

	//Portgroup state
	static std::map<uint16_t, portgroup_state*> portgroups;	
	
	//Handle mutual exclusion over the portgroup state
	static pthread_mutex_t mutex;

	/*
	* Scheduling routines (no thread safe)
	*/
	static inline unsigned int rx_sched(){
		unsigned int grp_id = next_rx_sched_grp_id;
		next_rx_sched_grp_id = (next_rx_sched_grp_id+1)%num_of_rx_groups;
		return grp_id;
	}
	static inline unsigned int tx_sched(){
		unsigned int _grp_id = next_tx_sched_grp_id;
		next_tx_sched_grp_id = (next_tx_sched_grp_id+1)%num_of_tx_groups;
		return _grp_id+num_of_rx_groups;
	}

	/*
	* Port mgmt (internal API)
	*/
	static rofl_result_t add_port_to_group(unsigned int grp_id, ioport* port);
	static rofl_result_t remove_port_from_group(unsigned int grp_id, ioport* port, bool mutex_locked=false);

	/* Start/Stop portgroup threads */
	static void start_portgroup_threads(portgroup_state* pg);
	static void stop_portgroup_threads(portgroup_state* pg);
	
	/*
	* Group mgmt
	*/
	static int create_group(pg_type_t type, unsigned int num_of_threads=DEFAULT_THREADS_PER_PG, bool mutex_locked=false);
	static rofl_result_t delete_group(unsigned int grp_id);
	static rofl_result_t delete_all_groups(void);
};

}// namespace xdpd::gnu_linux 
}// namespace xdpd


#endif /* IOMANAGER_H_ */
