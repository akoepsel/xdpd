/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef IOPORT_VLINK_H
#define IOPORT_VLINK_H 1

#include <unistd.h>
#include <rofl.h>
#include <rofl/datapath/pipeline/common/datapacket.h>
#include <rofl/datapath/pipeline/switch_port.h>
#include "../ioport.h" 
#include "../../datapacketx86.h" 


/**
* @file ioport_vlink.h
* @author Marc Sune<marc.sune (at) bisdn.de>
*
* @brief TODO 
*/

class ioport_vlink : public ioport{

public:
	//ioport_vlink
	ioport_vlink(switch_port_t* of_ps, unsigned int num_queues= IO_IFACE_NUM_QUEUES);
	virtual ~ioport_vlink();
	 
	//Set other vlink edge
	virtual void set_connected_port(ioport_vlink* connected_port);
	
	//Enque packet for transmission (blocking)
	virtual void enqueue_packet(datapacket_t* pkt, unsigned int q_id);

	//Non-blocking read and write
	virtual datapacket_t* read(void);
	virtual unsigned int write(unsigned int q_id, unsigned int num_of_buckets);

	//Get read&write fds. Return -1 if do not exist
	inline virtual int get_read_fd(void){return input[READ];};
	int get_fake_write_fd(void){return input[WRITE];};
	inline virtual int get_write_fd(void){return notify_pipe[READ];};

	virtual rofl_result_t 
	disable();

	virtual rofl_result_t
	enable();

protected:
	//fds
	int input[2];
	int notify_pipe[2];
	
	//Pipe extremes
	static const unsigned int READ=0;
	static const unsigned int WRITE=1;
	
};

#endif /* IOPORT_VLINK_H_ */
