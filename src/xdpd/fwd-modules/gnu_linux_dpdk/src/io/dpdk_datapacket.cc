#include "dpdk_datapacket.h"

datapacket_dpdk_t* create_datapacket_dpdk(void){
	
	datapacket_dpdk_t* dpkt = (datapacket_dpdk_t*) malloc(sizeof(datapacket_dpdk_t));
	dpkt->headers = init_classifier();
	dpkt->icmpv4_recalc_checksum = false;
	dpkt->ipv4_recalc_checksum = false;
	dpkt->tcp_recalc_checksum = false;
	dpkt->udp_recalc_checksum = false;
	dpkt->buffering_status = DPDK_DATAPACKET_BUFFER_IS_EMPTY;
	return dpkt;
	
}

void destroy_datapacket_dpdk(datapacket_dpdk_t* dpkt){
	
	destroy_classifier(dpkt->headers);
	free(dpkt);
	
}

rofl_result_t
init_datapacket_dpdk(
		datapacket_dpdk_t* dpkt,
		struct rte_mbuf* mbuf,
		of_switch_t* sw,
		uint32_t in_port,
		uint32_t in_phy_port,
		bool classify, 
		bool copy_packet_to_internal_buffer){
	
	uint8_t *buf = rte_pktmbuf_mtod(mbuf, uint8_t*);

	if( copy_packet_to_internal_buffer) {
		//NOTE is this needed?
		assert(0);
	}else{
		if(!buf)
			return ROFL_FAILURE;

		//NOTE NEEDED? init_internal_buffer_location_defaults(X86_DATAPACKET_BUFFERED_IN_NIC, buf, buflen);
	}

	//Fill the structure
	dpkt->lsw = sw;
	dpkt->in_port = in_port;
	dpkt->in_phy_port = in_phy_port;
	dpkt->output_queue = 0;

	// Timestamp S1
	TM_STAMP_STAGE_DPX86(dpkt, TM_S1);
	
	//Classify the packet
	if(classify)
		classify_packet(dpkt->headers, get_buffer_dpdk(dpkt), get_buffer_length_dpdk(dpkt));

	return ROFL_SUCCESS;
}

void
reset_datapacket_dpdk(datapacket_dpdk_t *dpkt){
	reset_classifier(dpkt->headers);

	if (DPDK_DATAPACKET_BUFFERED_IN_USER_SPACE == get_buffering_status_dpdk(dpkt)){
#ifndef NDEBUG
		// not really necessary, but makes debugging a little bit easier
		memset((uint8_t*)get_buffer_dpdk(dpkt), 0x00, get_buffer_length_dpdk(dpkt));
#endif
		dpkt->buffering_status = DPDK_DATAPACKET_BUFFER_IS_EMPTY;
	}
}



/*
 * Push&pop operations
 */
rofl_result_t
push_datapacket_offset(datapacket_dpdk_t *dpkt, unsigned int offset, unsigned int num_of_bytes){
	
	uint8_t *src_ptr = get_buffer_dpdk(dpkt);
	uint8_t *dst_ptr = (uint8_t*)rte_pktmbuf_prepend(dpkt->mbuf, num_of_bytes);
	//NOTE dst_ptr = src_ptr - num_of_bytes
	
	if( NULL==dst_ptr )
		return ROFL_FAILURE;
	
	if(false==rte_pktmbuf_is_contiguous(dpkt->mbuf)){
		assert(0);
		return ROFL_FAILURE;
	}

	// move header num_of_bytes backward
	memmove(dst_ptr, src_ptr, offset);
	
#ifndef NDEBUG
	// initialize new pushed memory area with 0x00
	memset(dst_ptr + offset, 0x00, num_of_bytes);
#endif

	return ROFL_SUCCESS;
}


rofl_result_t
pop_datapacket_offset(datapacket_dpdk_t *dpkt, unsigned int offset, unsigned int num_of_bytes){
	
	uint8_t *src_ptr = get_buffer_dpdk(dpkt);
	uint8_t *dst_ptr = (uint8_t*)rte_pktmbuf_adj(dpkt->mbuf, num_of_bytes);
	//NOTE dst_ptr = src_ptr + num_of_bytes
	
	if( NULL==dst_ptr )
		return ROFL_FAILURE;

	if(false==rte_pktmbuf_is_contiguous(dpkt->mbuf)){
		assert(0);
		return ROFL_FAILURE;
	}
	
	// move first bytes backward
	memmove(dst_ptr, src_ptr, offset);
	
#ifndef NDEBUG
	// set now unused bytes to 0x00 for easier debugging
	memset(src_ptr, 0x00, num_of_bytes);
#endif

	return ROFL_SUCCESS;
}


rofl_result_t
push_datapacket_point(datapacket_dpdk_t *dpkt, uint8_t* push_point, unsigned int num_of_bytes){
	
	uint8_t *src_ptr = get_buffer_dpdk(dpkt);
	
	if(false==rte_pktmbuf_is_contiguous(dpkt->mbuf)){
		assert(0);
		return ROFL_FAILURE;
	}
	
	if (push_point < src_ptr){
		return ROFL_FAILURE;
	}

	if (((uint8_t*)push_point + num_of_bytes) > (src_ptr + get_buffer_length_dpdk(dpkt))){
		return ROFL_FAILURE;
	}

	size_t offset = (push_point - src_ptr);

	return push_datapacket_offset(dpkt, offset, num_of_bytes);
}



rofl_result_t
pop_datapacket_point(datapacket_dpdk_t *dpkt, uint8_t* pop_point, unsigned int num_of_bytes){
	
	uint8_t *src_ptr = get_buffer_dpdk(dpkt);
	
	if(false==rte_pktmbuf_is_contiguous(dpkt->mbuf)){
		assert(0);
		return ROFL_FAILURE;
	}
	
	if (pop_point < src_ptr){
		return ROFL_FAILURE;
	}

	if (((uint8_t*)pop_point + num_of_bytes) > (src_ptr + get_buffer_length_dpdk(dpkt))){
		return ROFL_FAILURE;
	}

	size_t offset = (src_ptr - pop_point);

	return pop_datapacket_offset(dpkt, offset, num_of_bytes);
}
