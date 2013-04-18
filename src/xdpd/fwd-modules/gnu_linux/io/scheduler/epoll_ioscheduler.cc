#include "epoll_ioscheduler.h"

#include <errno.h>
#include <unistd.h>

#include <rofl/datapath/pipeline/common/datapacket.h>
#include <rofl/datapath/pipeline/switch_port.h>
#include <rofl/datapath/pipeline/platform/cutil.h>
#include "../iomanager.h"
#include "../bufferpool.h"
#include "../../util/ringbuffer.h"
#include "../../ls_internal_state.h"

/*
* 
* Implements a simple WRR scheduling algorithm within port-group 
* To be further explored other I/O algorithms
*
*/

//Static members initialization
const unsigned int epoll_ioscheduler::READ_BUCKETS[3]={epoll_ioscheduler::READ_BUCKETSPP, epoll_ioscheduler::READ_BUCKETSPP*2, epoll_ioscheduler::READ_BUCKETSPP*3};
const unsigned int epoll_ioscheduler::WRITE_BUCKETS[3]= {epoll_ioscheduler::WRITE_BUCKETSPP, epoll_ioscheduler::WRITE_BUCKETSPP*2, epoll_ioscheduler::WRITE_BUCKETSPP*3};
const float epoll_ioscheduler::WRITE_QOS_QUEUE_FACTOR[4]={1,1.2,1.5,2}; //TODO: PORT_MAX_NUMBER_OF_QUEUES
#ifdef DEBUG
bool epoll_ioscheduler::by_pass_processing = false;
#endif
/*
* Call port based on scheduling algorithm 
*/

inline void epoll_ioscheduler::process_port_io(ioport* port){

	unsigned int i, q_id, n_buckets;
	datapacket_t* pkt;
	
	if(!port || !port->of_port_state)
		return;

	//Process input(up to READ_BUCKETS[buffer_state])
	n_buckets = READ_BUCKETS[port->get_input_queue_state()];

	//Perform up_to n_buckets_read
#ifdef DEBUG
	//std::cerr<<"Trying to read at port ["<<port->of_port_state->name<<"] with "<< n_buckets <<"buckets. Queue state: "<<port->get_input_queue_state()<<std::endl;
#endif
	for(i=0; i<n_buckets; i++){
		
		//Attempt to read (non-blocking)
		//do {
		pkt = port->read();
		
		if(pkt){

#ifdef DEBUG
			if(by_pass_processing){
				//By-pass processing and schedule to write in the same port
				//Only used for testing
				port->enqueue_packet(pkt,0); //Push to queue 0
			}else{
#endif
				/*
				* Push packet to the logical switch queue. 
				* If not successful (congestion), drop it!
				*/

				int result = ((ringbuffer*)((struct logical_switch_internals*)port->of_port_state->attached_sw->platform_state)->ringbuffer)->non_blocking_write(pkt);
				if( result == ringbuffer::RB_FAILURE ){
					//XXX: check whether resources in the ioport (e.g. ioport_mmap) can be released only by that (maybe virtual function called by ioport)
					//std::cerr << "<" << __func__ << ":" << __LINE__ << "> FAILURE when writting to ringbuffer" << std::endl;
					//fprintf(stderr,"[%s] Packet DROPPED, processing buffer full (sw: %s)\n",port->of_port_state->name,port->of_port_state->attached_sw->name);
					bufferpool::release_buffer(pkt);
				}else{
					//DEBUG_CALL(std::cerr << "<" << __func__ << ":" << __LINE__ << "> SUCCESS when writting to ringbuffer->" << std::endl);
					//fprintf(stderr,"[%s] Packet scheduled for process -> sw: %s\n",port->of_port_state->name,port->of_port_state->attached_sw->name);
				}
					
#ifdef DEBUG
			}
#endif
		}else{
#ifdef DEBUG
			//std::cerr<<"Port reading: breaking at num_of_buckets "<<i<<std::endl;
#endif
			break;
		}
		//} while (pkt);
	}

	//Process output (up to WRITE_BUCKETS[output_queue_state])
	for(q_id=0; q_id < port->get_num_of_queues(); q_id++){
		
		//Increment number of buckets
		n_buckets = WRITE_BUCKETS[port->get_output_queue_state(q_id)]*WRITE_QOS_QUEUE_FACTOR[q_id];

#ifdef DEBUG
		//std::cerr<<"Trying to write at port queue: "<<q_id <<" with: "<< n_buckets <<" buckets. Queue state: "<<port->get_output_queue_state(q_id)<<std::endl;
#endif
		//Perform up to n_buckets write	
		port->write(q_id,n_buckets);
	}

}

/*
* EPOLL add fd
*/
inline void epoll_ioscheduler::add_fd_epoll(struct epoll_event* ev, int epfd, ioport* port, int fd){

	ev->events = EPOLLIN | EPOLLPRI | EPOLLET;
	ev->data.fd = fd;
	ev->data.ptr = (void*)port;

	if( epoll_ctl(epfd, EPOLL_CTL_ADD, fd, ev) < 0){
		//XXX FIXME  do something, trace or exit on development
		fprintf(stderr,"epoll failed");
	}
}
/*
* Initializes or updates the EPOLL file descriptor set (epoll_ctl)
*/
inline void epoll_ioscheduler::init_or_update_fds(portgroup_state* pg, int* epfd, struct epoll_event** ev, struct epoll_event** events, unsigned int* current_num_of_ports, unsigned int* current_hash ){

	unsigned int i;
	int fd;

	//If there are no running_ports just skip
	if(!pg->running_ports->size())
		return;

	//Destroy previous epoll instance, if any
	if(*epfd != -1){
		close(*epfd);	
		free(*ev);
		free(*events);
	}
	
	//Create epoll
	*epfd = epoll_create(pg->running_ports->size()*2);

	//lock running vector, so that we can safely iterate over it
	pg->running_ports->read_lock();

	//Allocate memory
	*ev = (epoll_event*)malloc( sizeof(struct epoll_event) * pg->running_ports->size()*2 );
	*events = (epoll_event*)malloc( sizeof(struct epoll_event) * pg->running_ports->size()*2 );

	if(!*ev){
		//FIXME: what todo...
		fprintf(stderr,"malloc failed");
		pg->running_ports->read_unlock();
		return;
	}

	//Assign current number_of_ports
	*current_num_of_ports = pg->running_ports->size();	

	for(i=0; i < *current_num_of_ports; i++){
		/* Read */
		fd = (*pg->running_ports)[i]->get_read_fd();
		if( fd != -1 )
			epoll_ioscheduler::add_fd_epoll( &((*ev)[(i*2)+READ]), *epfd, (*pg->running_ports)[i], fd);
		
		/* Write */
		fd = (*pg->running_ports)[i]->get_write_fd();
		if( fd != -1 )
			epoll_ioscheduler::add_fd_epoll(&((*ev)[(i*2)+WRITE]), *epfd, (*pg->running_ports)[i], fd);
	}

	//Assign current hash
	*current_hash = pg->running_hash;	

	//Signal as synchronized
	iomanager::signal_as_synchronized(pg);
	
	//unlock running vector
	pg->running_ports->read_unlock();
}


void* epoll_ioscheduler::process_io(void* grp){

	int epfd, res;
	struct epoll_event *ev=NULL, *events = NULL;
	unsigned int current_hash=0, current_num_of_ports=0;
	portgroup_state* pg = (portgroup_state*)grp;
 
	//Init epoll fd set
	epfd = -1;
	init_or_update_fds(pg, &epfd, &ev, &events, &current_num_of_ports, &current_hash );

	std::cerr<<"Initialization of epoll completed in thread:"<<(unsigned int)pthread_self()<<std::endl;
	
	/*
	* Infinite loop unless group is stopped. e.g. all ports detached
	*/
	while(iomanager::keep_on_working(pg)){

		//std::cerr<<"Epoll wait.."<<std::endl;

		//Wait for events or TIMEOUT_MS
		res = epoll_wait(epfd, events, current_num_of_ports, EPOLL_TIMEOUT_MS);
		
		//std::cerr<<"After epoll.."<<std::endl;
		
		if(res == -1){
#ifdef DEBUG
			//std::cerr<<"epoll failed"<<strerror(errno)<<" \n";
			//fprintf(stderr,"epoll failed");
#else
			continue;
#endif
		}
		if(res == 0){
			//std::cerr<<"Timeout.."<<std::endl;
			//Check if this is really necessary
			//Timeout loop over ALL fds 
			for(unsigned int i=0;i<current_num_of_ports*2;i+=2){
				//epoll_ioscheduler::process_port_io((ioport*)ev[i].data.ptr);
			}	
		}else{	
			//std::cerr<<"Active fds.."<<std::endl;
			//Loop over active fd
			//FIXME: skip double port process_port_io (both input&output fd signal)
			for(int i = 0;i<res;i++){
				epoll_ioscheduler::process_port_io((ioport*)events[i].data.ptr);
			}	
		}
		//Check for updates in the running ports 
		if( pg->running_hash != current_hash )
			init_or_update_fds(pg, &epfd, &ev,&events, &current_num_of_ports, &current_hash );
	}

	std::cout<<"Finishing execution of I/O thread: #"<<pthread_self()<<std::endl;

	//Free dynamic memory
	free(ev);
	free(events);

	//Return whatever
	pthread_exit(NULL);
}
