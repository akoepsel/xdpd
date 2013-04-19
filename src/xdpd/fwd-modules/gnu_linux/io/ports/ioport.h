/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef IOPORT_H
#define IOPORT_H 

#include <string>

#include <rofl.h>
#include <rofl/datapath/pipeline/common/datapacket.h>
#include <rofl/datapath/pipeline/switch_port.h>
#include "../../util/ringbuffer.h" 

/**
* @file ioport.h
* @author Marc Sune<marc.sune (at) bisdn.de>
*
* @brief Abstract class representing a network interface (port)
*
*/

class ioport{

public:
	/**
	* @brief Constructor for the ioport
	* Constructs the ioport. The state of the port is explicitely undefined
	* (usually down) unless enable/disable is called AFTER the constructor.
	*/
	ioport(switch_port_t* of_ps, unsigned int q_num=MAX_OUTPUT_QUEUES);

	/**
	* Destructor
	*/
	virtual ~ioport(void);

	/**
	* Retrieve the number of queues this port supports. 0 lowest priority.
	* This method is thread safe and shall be invoked, generally speaking,
	* by the threads in charge of processing data packets. This method shall
	* NOT be called by an I/O thread 
	*/
	inline unsigned int get_num_of_queues(void){ return num_of_queues; } 

	/**
	* Retrieve the size (in number of slots) of a queue 
	*/
	inline unsigned int get_queue_size(unsigned int id){ 
		if(id < num_of_queues)
			return output_queues[id].MAX_SLOTS; 
		
		return 0;
	} 


	/**
	* Enque packet for transmission(this is blocking call)
	*/
	virtual void enqueue_packet(datapacket_t* pkt, unsigned int q_id)=0;

	//Non-blocking read and write
	/**
	* @brief Read(RX) one (1) packet if available, return NULL if no packet
	* can be read immediately. 
	*
	* This method must be NON-BLOCKING
	*/
	virtual datapacket_t* read(void)=0;

	/**
	* @brief Write in the wire up to up_to_buckets number of packets from queue q_id
	*
	* The packets shall be read from the output_queues[q_id]. If no more packets
	* are currently present on the queue, the method shall immediately return.
	*
	* This method must be NON-BLOCKING
	*
	* @return up_to_buckets - (number of buckets used)
	*/
	virtual unsigned int write(unsigned int q_id, unsigned int up_to_buckets)=0;

	//Get read&write fds. Return -1 if do not exist
	virtual int get_read_fd(void)=0;
	virtual int get_write_fd(void)=0;

	//Get buffer status; generally used to create "smart" schedulers
	virtual ringbuffer_state_t get_input_queue_state(void); 
	virtual ringbuffer_state_t get_output_queue_state(unsigned int q_id=0); 

	/**
	* @brief Retrieves the number of buffers required by the port to be operating at line-rate; 
	* must be power of 2 
	*/
	virtual unsigned int get_required_buffers(void){ return NUM_OF_REQUIRED_BUFFERS;}; 
	
	/**
	 * Sets the port administratively up. This MUST change the of_port_state appropiately
	 */
	virtual rofl_result_t enable(void)=0;

	/**
	 * Sets the port administratively down. This MUST change the of_port_state appropiately
	 */
	virtual rofl_result_t disable(void)=0;

	/**
	 * Sets the port receiving behaviour. This MUST change the of_port_state appropiately
	 * Inherited classes may override this method if they have specific things to do.
	 */
	virtual rofl_result_t set_drop_received_config(bool drop_received);

	/**
	 * Sets the port output behaviour. This MUST change the of_port_state appropiately
	 * Inherited classes may override this method if they have specific things to do.
	 */
	virtual rofl_result_t set_forward_config(bool forward_packets);

	/**
	 * Sets the port Openflow specific behaviour for non matching packets (PACKET_IN). This MUST change the of_port_state appropiately
	 * Inherited classes may override this method if they have specific things to do.
	 */
	virtual rofl_result_t set_generate_packet_in_config(bool generate_pkt_in);

	/**
	 * Sets the port advertised features. This MUST change the of_port_state appropiately
	 * Inherited classes may override this method if they have specific things to do.
	 */
	virtual rofl_result_t set_advertise_config(uint32_t advertised);


	//Port state (rofl-pipeline port state reference)
	switch_port_t* of_port_state;

protected:
	static const unsigned int MAX_OUTPUT_QUEUES=8;	/*!< Constant max output queues */
	static const unsigned int NUM_OF_REQUIRED_BUFFERS=2048;	/* Required buffers for the port to operate at line rate */
	
	//Output QoS queues
	unsigned int num_of_queues;

	/**
	* @brief Output (TX) queues (num_of_queues) 
	* 
	* The output queues is an array of queues (output_queues[num_of_queues])
	* for QoS purposes (set-queue). output_queues[0] is always the queue with 
	* least priority (best effort)
	*/
	ringbuffer* output_queues;

	/**
	* Input queue (intermediate-buffering). 
	* 
	* This queue might not always be used by the ioports, if they don't need a intermediate
	* buffering of packets. If it is not used, the port shall attempt to enqueue packets
	* to the appropiate LS processing queue.
	*
	*/
	ringbuffer* input_queue;
};

#endif /* IOPORT_H_ */
