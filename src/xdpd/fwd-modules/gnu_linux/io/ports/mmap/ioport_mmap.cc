#include "ioport_mmap.h"
#include "../../bufferpool.h"
#include "../../datapacketx86.h"

#include <rofl/common/utils/c_logger.h>
#include <rofl/common/protocols/fetherframe.h>
#include <rofl/common/protocols/fvlanframe.h>

using namespace rofl;

//Constructor and destructor
ioport_mmap::ioport_mmap(
		/*int port_no,*/
		switch_port_t* of_ps,
		int block_size,
		int n_blocks,
		int frame_size,
		unsigned int num_queues) :
			ioport(of_ps, num_queues),
			rx(PACKET_RX_RING, std::string(of_ps->name), 2 * block_size, n_blocks, frame_size),
			tx(PACKET_TX_RING, std::string(of_ps->name), block_size, n_blocks, frame_size),
			hwaddr(of_ps->hwaddr, OFP_ETH_ALEN)
{
	int ret;

	//Open pipe for output signaling on enqueue	
	ret = pipe(notify_pipe);
	(void)ret; // todo use the value

	//Set non-blocking read/write in the pipe
	for(unsigned int i=0;i<2;i++){
		int flags = fcntl(notify_pipe[i], F_GETFL, 0);	///get current file status flags
		flags |= O_NONBLOCK;				//turn off blocking flag
		fcntl(notify_pipe[i], F_SETFL, flags);		//set up non-blocking read
	}
}


ioport_mmap::~ioport_mmap()
{
	close(notify_pipe[READ]);
	close(notify_pipe[WRITE]);
}

//Read and write methods over port
void
ioport_mmap::enqueue_packet(datapacket_t* pkt, unsigned int q_id)
{
	//Whatever
	const char c='a';
	int ret;
	

	if (of_port_state->up && of_port_state->forward_packets ) {
		//Store on queue and exit. This is NOT copying it to the mmap buffer
		output_queues[q_id].blocking_write(pkt);

		ROFL_DEBUG_VERBOSE("%s(): Enqueued, buffer size: %d\n", __FUNCTION__, output_queues[q_id].size());
	
		//TODO: make it happen only if thread is really sleeping...
		ret = ::write(notify_pipe[WRITE],&c,sizeof(c));
		(void)ret; // todo use the value
	} else {
		ROFL_DEBUG_VERBOSE("%s(): drop packet %p scheduled for queue %u\n", __FUNCTION__, pkt, q_id);
		// port down -> drop packet
		bufferpool::release_buffer(pkt);
	}

}

// handle read
datapacket_t*
ioport_mmap::read()
{
	//Whatever
	char c;
	int ret;

	if(!of_port_state->up || of_port_state->drop_received)
		return NULL;

	//Just take the byte from the pipe	
	ret = ::read(notify_pipe[READ],&c,sizeof(c));
	(void)ret; // todo use the value
	
	if (input_queue->is_empty()) {
		read_loop(rx.sd, 1);
	}

	return input_queue->non_blocking_read();
}

int
ioport_mmap::read_loop(int fd /* todo do we really need the fd? */,
		int read_max)
{
	if (rx.sd != fd)
	{
		return 0;
	}

#if 0
	ROFL_DEBUG_VERBOSE("ioport_mmap(%s)::read_loop() total #slots:%d\n",
			of_port_state->name, rx.req.tp_frame_nr);
#endif
	int cnt = 0;
	int rx_bytes_local = 0;

	while (cnt < read_max) {

		// i = (i == rxline.req.tp_frame_nr - 1) ? 0 : (i+1);

		/*
		   Frame structure:

		   - Start. Frame must be aligned to TPACKET_ALIGNMENT=16
		   - struct tpacket_hdr
		   - pad to TPACKET_ALIGNMENT=16
		   - struct sockaddr_ll
		   - Gap, chosen so that packet data (Start+tp_net) aligns to
		     TPACKET_ALIGNMENT=16
		   - Start+tp_mac: [ Optional MAC header ]
		   - Start+tp_net: Packet data, aligned to TPACKET_ALIGNMENT=16.
		   - Pad to align to TPACKET_ALIGNMENT=16
		 */

		struct tpacket2_hdr *hdr = rx.read_packet();
		if (NULL == hdr) {
			break;
		}

		/* sanity check */
		if (hdr->tp_mac + hdr->tp_snaplen > rx.req.tp_frame_size) {
			ROFL_DEBUG_VERBOSE("sanity check during read mmap failed\n");
			
			//Increment error statistics
			switch_port_stats_inc(of_port_state,0,0,0,0,1,0);

			return cnt;
		}

		// todo check if this is necessary
//		fetherframe ether(((uint8_t*)hdr + hdr->tp_mac), hdr->tp_len);
//		if (ether.get_dl_src() == hwaddr)
//		{
//			ROFL_DEBUG_VERBOSE("cioport(%s)::handle_revent() self-originating "
//				"frame rcvd in slot i:%d, ignoring", devname.c_str(), i);
//			continue; // ignore self-originating frames
//		}

		struct sockaddr_ll *sll = (struct sockaddr_ll*)((uint8_t*)hdr + TPACKET_ALIGN(sizeof(struct tpacket_hdr)));
		if (PACKET_OUTGOING == sll->sll_pkttype) {
			ROFL_DEBUG_VERBOSE("cioport(%s)::handle_revent() outgoing "
					"frame rcvd in slot i:%d, ignoring\n", of_port_state->name, rx.rpos);
			goto next; // ignore outgoing frames
		}

		{
			//Retrieve buffer from pool: this is a blocking call
			datapacket_t *pkt = bufferpool::get_free_buffer(false);

			// handle no free buffer
			if (NULL == pkt) {
				//Increment error statistics
				switch_port_stats_inc(of_port_state,0,0,0,0,1,0);
		
				return cnt;
			}

			datapacketx86 *pkt_x86 = (datapacketx86*) pkt->platform_state;

			cmacaddr eth_src = cmacaddr(((struct fetherframe::eth_hdr_t*)((uint8_t*)hdr + hdr->tp_mac))->dl_src, OFP_ETH_ALEN);

			if (hwaddr == eth_src) {
				ROFL_DEBUG_VERBOSE("cioport(%s)::handle_revent() outgoing "
						"frame rcvd in slot i:%d, src-mac == own-mac, ignoring\n", of_port_state->name, rx.rpos);
				pkt_x86->destroy();
				bufferpool::release_buffer(pkt);
				goto next; // ignore outgoing frames
			}


			if (0 != hdr->tp_vlan_tci) {
				// packet has vlan tag
				pkt_x86->init(NULL, hdr->tp_len + sizeof(struct fvlanframe::vlan_hdr_t), of_port_state->attached_sw, get_port_no());

				// write ethernet header
				memcpy(pkt_x86->get_buffer(), (uint8_t*)hdr + hdr->tp_mac, sizeof(struct fetherframe::eth_hdr_t));

				// set dl_type to vlan
				if ( htobe16(ETH_P_8021Q) == ((struct fetherframe::eth_hdr_t*)((uint8_t*)hdr + hdr->tp_mac))->dl_type ) {
					((struct fetherframe::eth_hdr_t*)pkt_x86->get_buffer())->dl_type = htobe16(ETH_P_8021Q); // tdoo maybe this should be ETH_P_8021AD
				} else {
					((struct fetherframe::eth_hdr_t*)pkt_x86->get_buffer())->dl_type = htobe16(ETH_P_8021Q);
				}

				// write vlan
				struct fvlanframe::vlan_hdr_t* vlanptr =
						(struct fvlanframe::vlan_hdr_t*) (pkt_x86->get_buffer()
								+ sizeof(struct fetherframe::eth_hdr_t));
				vlanptr->byte0 =  (hdr->tp_vlan_tci >> 8);
				vlanptr->byte1 = hdr->tp_vlan_tci & 0x00ff;
				vlanptr->dl_type = ((struct fetherframe::eth_hdr_t*)((uint8_t*)hdr + hdr->tp_mac))->dl_type;

				// write payload
				memcpy(pkt_x86->get_buffer() + sizeof(struct fetherframe::eth_hdr_t) + sizeof(struct fvlanframe::vlan_hdr_t),
						(uint8_t*)hdr + hdr->tp_mac + sizeof(struct fetherframe::eth_hdr_t),
						hdr->tp_len - sizeof(struct fetherframe::eth_hdr_t));
                
                pkt_x86->headers->classify();

			} else {
				// no vlan tag present
				pkt_x86->init((uint8_t*)hdr + hdr->tp_mac, hdr->tp_len, of_port_state->attached_sw, get_port_no());
			}

			// fill input_queue
			input_queue->non_blocking_write(pkt);
		}

		rx_bytes_local += hdr->tp_len;
		cnt++;

next:
		hdr->tp_status = TP_STATUS_KERNEL; // return packet to kernel
		rx.rpos++; // select next packet. todo: should be moved to pktline
		if (rx.rpos >=rx.req.tp_frame_nr) {
			rx.rpos = 0;
		}
	}

	// ROFL_DEBUG_VERBOSE("cnt=%d\n", cnt);

	//Increment statistics
	if(cnt)
		switch_port_stats_inc(of_port_state, cnt, 0, rx_bytes_local, 0, 0, 0);	
	
	return cnt;
}

unsigned int
ioport_mmap::write(unsigned int q_id, unsigned int num_of_buckets)
{
	datapacket_t* pkt;
	unsigned int cnt = 0;
	int tx_bytes_local = 0;

	if (0 == output_queues[q_id].size()) {
		return num_of_buckets;
	}

	ROFL_DEBUG_VERBOSE("%s(q_id=%d, num_of_buckets=%u): on %s with queue.size()=%u\n",
			__FUNCTION__,
			q_id,
			num_of_buckets,
			of_port_state->name,
			output_queues[q_id].size());

	// read available packets from incoming buffer
	for ( ; 0 < num_of_buckets; --num_of_buckets ) {

		pkt = output_queues[q_id].non_blocking_read();

		if (NULL == pkt) {
			ROFL_DEBUG_VERBOSE("%s(): on %s: no packet in output_queue %u left, %u buckets left\n",
					__FUNCTION__,
					of_port_state->name,
					q_id,
					num_of_buckets);
			break;
		}

		/*
		 Frame structure:

		 - Start. Frame must be aligned to TPACKET_ALIGNMENT=16
		 - struct tpacket_hdr
		 - pad to TPACKET_ALIGNMENT=16
		 - struct sockaddr_ll
		 - Gap, chosen so that packet data (Start+tp_net) aligns to
		 TPACKET_ALIGNMENT=16
		 - Start+tp_mac: [ Optional MAC header ]
		 - Start+tp_net: Packet data, aligned to TPACKET_ALIGNMENT=16.
		 - Pad to align to TPACKET_ALIGNMENT=16
		 */
		struct tpacket2_hdr *hdr = tx.get_free_slot();

		if (NULL != hdr) {
			datapacketx86* pkt_x86;
			// Recover x86 specific state
			pkt_x86 = (datapacketx86*) pkt->platform_state;

			// todo check the right size

			tx.copy_packet(hdr, pkt_x86);

			// todo statistics
			tx_bytes_local += hdr->tp_len;

		} else {
			ROFL_DEBUG_VERBOSE("no free slot in ringbuffer\n");

			//Increment error statistics
			switch_port_stats_inc(of_port_state,0,0,0,0,0,1);


			// Release and exit
			bufferpool::release_buffer(pkt);
			break;
		}

		// pkt is processed
		bufferpool::release_buffer(pkt);
		cnt++;
	} // for

#if 0
	ROFL_DEBUG_VERBOSE("ioport_mmap::write(q_id=%d, num_of_buckets=%u) on %s all packets scheduled\n",
			q_id,
			num_of_buckets,
			of_port_state->name);
#endif

	if (cnt) {
		ROFL_DEBUG_VERBOSE("%s(): schedule %u packet(s) to be send\n", __FUNCTION__, cnt);
		// send packet-ins tx queue (this call is currently a blocking call!)
		tx.send();

		//Increment statistics
		switch_port_stats_inc(of_port_state, 0, cnt, 0, tx_bytes_local, 0, 0);	
	}

	// return not used buckets
	return num_of_buckets;
}

/*
*
* Enable and disable port routines
*
*/
rofl_result_t
ioport_mmap::enable() {
	struct ifreq ifr;
	int sd, rc;

	if ((sd = socket(AF_PACKET, SOCK_RAW, 0)) < 0) {
		return ROFL_FAILURE;
	}

	memset(&ifr, 0, sizeof(struct ifreq));
	strcpy(ifr.ifr_name, of_port_state->name);

	if ((rc = ioctl(sd, SIOCGIFINDEX, &ifr)) < 0) {
		return ROFL_FAILURE;
	}

	if ((rc = ioctl(sd, SIOCGIFFLAGS, &ifr)) < 0) {
		close(sd);
		return ROFL_FAILURE;
	}

	if (IFF_UP & ifr.ifr_flags) {
		close(sd);
		//Already up.. Silently skip
		return ROFL_SUCCESS;
	}

	ifr.ifr_flags |= IFF_UP;

	if ((rc = ioctl(sd, SIOCSIFFLAGS, &ifr)) < 0) {
		close(sd);
		return ROFL_FAILURE;
	}

	// enable promiscous mode
	bzero((void*)&ifr, sizeof(ifr));
	strncpy(ifr.ifr_name, of_port_state->name, sizeof(ifr.ifr_name));
	if ((rc = ioctl(sd, SIOCGIFFLAGS, &ifr)) < 0)
	{
		throw ePktLineFailed();
	}
	ifr.ifr_flags |= IFF_PROMISC;
	if ((rc = ioctl(sd, SIOCSIFFLAGS, &ifr)) < 0)
	{
		throw ePktLineFailed();
	}

	// todo recheck?
	// todo check link state IFF_RUNNING
	of_port_state->up = true;

	close(sd);
	return ROFL_SUCCESS;
}

rofl_result_t
ioport_mmap::disable() {
	struct ifreq ifr;
	int sd, rc;

	if ((sd = socket(AF_PACKET, SOCK_RAW, 0)) < 0) {
		return ROFL_FAILURE;
	}

	memset(&ifr, 0, sizeof(struct ifreq));
	strcpy(ifr.ifr_name, of_port_state->name);

	if ((rc = ioctl(sd, SIOCGIFINDEX, &ifr)) < 0) {
		return ROFL_FAILURE;
	}

	if ((rc = ioctl(sd, SIOCGIFFLAGS, &ifr)) < 0) {
		close(sd);
		return ROFL_FAILURE;
	}

	if ( !(IFF_UP & ifr.ifr_flags) ) {
		close(sd);
		//Already down.. Silently skip
		return ROFL_SUCCESS;
	}

	ifr.ifr_flags &= ~IFF_UP;

	if ((rc = ioctl(sd, SIOCSIFFLAGS, &ifr)) < 0) {
		close(sd);
		return ROFL_FAILURE;
	}

	// todo recheck?
	// todo check link state IFF_RUNNING
	of_port_state->up = false;

	close(sd);

	return ROFL_SUCCESS;
}
