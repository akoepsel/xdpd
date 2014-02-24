#include "pktin_dispatcher.h"
#include <rofl/common/utils/c_logger.h>
#include <rofl/datapath/afa/openflow/openflow1x/of1x_cmm.h>

#include "bufferpool.h"
#include "datapacket_storage.h"
#include "dpdk_datapacket.h"

sem_t pktin_sem;
struct rte_ring* pkt_ins;
bool keep_on_pktins;
bool destroying_sw;
static pthread_t pktin_thread;
static pthread_mutex_t pktin_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t drain_mutex=PTHREAD_MUTEX_INITIALIZER;

using namespace xdpd::gnu_linux;

//MBUF pool
extern struct rte_mempool* pool_direct;

//Process packet_ins
static void* process_packet_ins(void* param){

	datapacket_t* pkt;
	datapacket_dpdk_t* pkt_dpdk;
	afa_result_t rv;
	unsigned int pkt_size;
	storeid id;
	of1x_switch_t* sw;	
	datapacket_storage* dps;
	struct rte_mbuf* mbuf;
	int drain_credits = -1; 

	//prepare timeout
	struct timespec timeout;

	while(likely(keep_on_pktins)){

		//Wait for incomming pkts
		clock_gettime(CLOCK_REALTIME, &timeout);
    		timeout.tv_sec += 1; 
		if( sem_timedwait(&pktin_sem, &timeout)  == ETIMEDOUT )
			continue;	

		//Dequeue
		if(rte_ring_sc_dequeue(pkt_ins, (void**)&pkt) != 0)
			continue;
	
		if(drain_credits > 0)
			drain_credits--;

		if(destroying_sw == true){
			if(drain_credits == -1){
				//Recover the number of credits that need to be decremented.
				while(sem_getvalue(&pktin_sem, &drain_credits) != 0);	
			}

			if(drain_credits == 0){
				//We have all credits drained
				//Reset the drain_credits flag and destroying_sw
				drain_credits = -1;
				destroying_sw = false;		
	
				//Wake up the caller of wait_pktin_draining() 
				pthread_mutex_unlock(&pktin_mutex);
			}
		}

		//Recover platform state
		pkt_dpdk = (datapacket_dpdk_t*)pkt->platform_state;
		mbuf = ((datapacket_dpdk_t*)pkt->platform_state)->mbuf;
		sw = (of1x_switch_t*)pkt->sw;
		dps = (datapacket_storage*)pkt->sw->platform_state;

		ROFL_DEBUG("Processing PKT_IN for packet(%p), mbuf %p, switch %p\n", pkt, mbuf, sw);
		//Store packet in the storage system. Packet is NOT returned to the bufferpool
		id = dps->store_packet(pkt);

		if(unlikely(id == datapacket_storage::ERROR)){
			ROFL_DEBUG("PKT_IN for packet(%p) could not be stored in the storage. Dropping..\n",pkt);

			//Return mbuf to the pool
			rte_pktmbuf_free(mbuf);

			//Return to the bufferpool
			bufferpool::release_buffer(pkt);
			continue;
		}

		//Normalize size
		pkt_size = get_buffer_length_dpdk(pkt_dpdk);
		if(pkt_size > sw->pipeline.miss_send_len)
			pkt_size = sw->pipeline.miss_send_len;
			
		//Process packet in
		rv = cmm_process_of1x_packet_in(sw->dpid, 
						pkt_dpdk->pktin_table_id, 	
						pkt_dpdk->pktin_reason, 	
						pkt_dpdk->in_port, 
						id, 	
						get_buffer_dpdk(pkt_dpdk), 
						pkt_size,
						get_buffer_length_dpdk(pkt_dpdk),
						((packet_matches_t*)&pkt->matches)
				);

		if( rv != AFA_SUCCESS ){
			ROFL_DEBUG("PKT_IN for packet(%p) could not be sent to sw:%s controller. Dropping..\n",pkt,sw->name);
			//Take packet out from the storage
			pkt = dps->get_packet(id);

			//Return mbuf to the pool
			rte_pktmbuf_free(mbuf);

			//Return to the bufferpool
			bufferpool::release_buffer(pkt);
		}
	}

	return NULL;
}

void wait_pktin_draining(of_switch_t* sw){

	//
	// Here we have to make sure that we are not returning
	// until all the PKT_INs belonging to the LSI have been
	// processed (dropped by the CMM)
	//
	// Note that *no more packets* will enqueued in the global
	// PKT_IN queue, since ports shall be descheduled.
	//
	// Strategy is just to make sure all current pending PKT_INs
	// are processed and return
	//

	//Serialize calls to wait_pktin_draining
	pthread_mutex_lock(&drain_mutex);

	//Signal PKT_IN thread that should unblock us when all current credits
	//have been drained 
	destroying_sw = true;
	pthread_mutex_lock(&pktin_mutex);
	
	//Serialize calls to wait_pktin_draining
	pthread_mutex_unlock(&drain_mutex);
}

// Launch pkt in thread
rofl_result_t pktin_dispatcher_init(){

	keep_on_pktins = true;

	if(sem_init(&pktin_sem, 0,0) < 0){
		return ROFL_FAILURE;
	}	

	//PKT_IN queue
	pkt_ins = rte_ring_create("PKT_IN_RING", IO_PKT_IN_STORAGE_MAX_BUF, SOCKET_ID_ANY, 0x0);
	
	if(!pkt_ins){
		ROFL_ERR("Unable to create PKT_INs ring queue\n");
		return ROFL_FAILURE;
	}

	//Reset synchronization flag 
	destroying_sw = false;	

	//Launch thread
	//XXX: use rte?
	if(pthread_create(&pktin_thread, NULL, process_packet_ins, NULL)<0){
		ROFL_ERR("pthread_create failed, errno(%d): %s\n", errno, strerror(errno));
		return ROFL_FAILURE;
	}

	return ROFL_SUCCESS;
}

//Stop and destroy packet in dispatcher
rofl_result_t pktin_dispatcher_destroy(){
	
	keep_on_pktins = false;
	pthread_join(pktin_thread,NULL);
	
	//Wait for thread to stop
	//XXX: use rte?
	
	//Destroy ring?
	//XXX: not available??
	
	return ROFL_SUCCESS;
}
