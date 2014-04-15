#include "flow_entry.h"

//Other headers
#include <arpa/inet.h>
#include "netfpga.h"
#include "../util/crc32.h"
#include <rofl/common/utils/c_logger.h>


bool check_mac_mask(netfpga_align_mac_addr_t* mac){	
	int i=0	;
	ROFL_DEBUG("MAC ADDRESS MASK: ");
	for (i=0; i<6;i++ ){
		ROFL_DEBUG("%x:",mac->addr[i]);
	}
	for (i=0; i<6;i++ ){
		if (mac->addr[i]!=0xFF) return false;
	}
	return true;

}

void fill_up_mac(netfpga_align_mac_addr_t* mac){	
	int i=0	;
	ROFL_DEBUG("MAC ADDRESS before filling up: ");
	for (i=0; i<6;i++ ){
		ROFL_DEBUG("%x:",mac->addr[i]);
	}
	
	uint32_t sum=0;
	for (i=0; i<6;i++ ){
		sum=sum+mac->addr[i];
	}
	if(sum==0){
		for (i=0; i<6;i++ ){
			mac->addr[i]=0xFF;
		}
		ROFL_DEBUG("MAC ADDRESS filled up ");
	}
}



//Creates a (empty) flow entry (mappable to HW) 
netfpga_flow_entry_t* netfpga_init_flow_entry(){

	netfpga_flow_entry_t* entry;
	
	entry = malloc(sizeof(netfpga_flow_entry_t));
	if(!entry)
		return NULL;

	//memset entry
	memset(entry,0,sizeof(*entry));
	
	//Allocate matches
	entry->matches = malloc(sizeof(netfpga_flow_entry_matches_t));
	if(!entry->matches){
		free(entry);
		return NULL;	
	}
	
	//Allocate wildcards
	entry->masks = malloc(sizeof(netfpga_flow_entry_matches_mask_t));
	if(!entry->masks){
		free(entry->matches);
		free(entry);
		return NULL;	
	}

	//Allocate actions	
	entry->actions = malloc(sizeof(netfpga_flow_entry_actions_t));
	if(!entry->actions){
		free(entry->matches);
		free(entry->masks);
		free(entry);
		return NULL;	
	}
	
	//Init all	
	memset(entry->matches, 0, sizeof(*(entry->matches)));
	memset(entry->masks, 0, sizeof(*(entry->masks)));
	memset(entry->actions, 0, sizeof(*(entry->actions)));

	ROFL_DEBUG("size of entry: %d",sizeof(entry));
	
	return entry;
}


//Destroys an entry previously created via netfpga_init_entry() 
void netfpga_destroy_flow_entry(netfpga_flow_entry_t* entry){

	if(!entry)
		return;
	
	free(entry->matches);
	free(entry->masks);
	free(entry->actions);
	free(entry);
}

//
// Flow mod translation
//
#define VID_BITMASK 0x0FFF
#define VID_PCP_BITMASK 0xE000
#define VID_PCP_SHIFT_BITS 13
#define TOS_ECN_BITMASK 0x03
#define TOS_DSCP_BITMASK 0xFC
#define TOS_DSCP_SHIFT_BITS 2 

#define MAX_NUM_OF_MATCHES 12

static rofl_result_t netfpga_flow_entry_map_matches(netfpga_flow_entry_t* entry, of1x_flow_entry_t* of1x_entry){
	
	of1x_match_t* match;
	int i;
	netfpga_flow_entry_matches_t* matches = entry->matches;
	netfpga_flow_entry_matches_mask_t* masks = entry->masks;
	netfpga_align_mac_addr_t *tmp, *tmp_mask;
	uint32_t tmp_ipv4_mask;
	int num_of_matches = 0;

	memset(masks, 0xFF, sizeof(*(masks)));

	ROFL_DEBUG("%s  %d num_of_matches: %x",__FILE__, __LINE__,of1x_entry->matches.num_elements);

	//Go through all the matches and set entry matches
	for(match = of1x_entry->matches.head; match;match = match->next){
		ROFL_DEBUG("%s  %d  of1x_entry->type : %x, ",__FILE__, __LINE__, match->type);
		switch(match->type){

			case OF1X_MATCH_IN_PORT:
				matches->src_port = 0x1 << (/*htons*/( (match->value->value.u32) - NETFPGA_PORT_BASE) *2);
				masks->src_port = 0x00; //Exact
				num_of_matches++;
				break;
 			case OF1X_MATCH_ETH_DST:
				tmp = (netfpga_align_mac_addr_t*) &(match->value->value.u64);
				tmp_mask = (netfpga_align_mac_addr_t*) &(match->value->value.u64);

				//inversion
				for (i=0;i<6;i++){
					matches->eth_dst.addr[i]=tmp->addr[5-i];
					masks->eth_dst.addr[i]=tmp_mask->addr[5-i];
				}
				
				if(check_mac_mask(tmp_mask)){
					//Is wildcarded
					memset(&(masks->eth_dst),0x00,sizeof(masks->eth_dst));  				
					
				}else
					num_of_matches++;
				
				//TODO: add mask...
				break;
 			case OF1X_MATCH_ETH_SRC:
				tmp = (netfpga_align_mac_addr_t*) &(match->value->value.u64);
				tmp_mask = (netfpga_align_mac_addr_t*) &(match->value->value.u64);

				//inversion
				for (i=0;i<6;i++){
					matches->eth_src.addr[i]=tmp->addr[5-i];
					masks->eth_src.addr[i]=tmp_mask->addr[5-i];
				}
				
				if(check_mac_mask(tmp_mask)){
					//Is wildcarded
					memset(&(masks->eth_src),0x00,sizeof(masks->eth_src));
				}else
					num_of_matches++;

				break;
 			case OF1X_MATCH_ETH_TYPE:
				matches->eth_type = htobe16( match->value->value.u16 );	
				if(  (match->value->mask.u16)==0xFFFF ){
					//Is wildcarded
					memset(&masks->eth_type,0x00,sizeof(masks->eth_type)); 
				}
				
				num_of_matches++;
				break;
 			case OF1X_MATCH_VLAN_PCP:
				matches->vlan_id = (match->value->value.u8 << VID_PCP_SHIFT_BITS) &VID_PCP_BITMASK;
				masks->vlan_id |= 0x7<< VID_PCP_SHIFT_BITS;
				num_of_matches++;
 				break;	
			case OF1X_MATCH_VLAN_VID:
				matches->vlan_id = htobe16(match->value->value.u16);
				masks->vlan_id |= VID_BITMASK; //Exact
				num_of_matches++;
				break;
 			case OF1X_MATCH_IP_DSCP:
				matches->ip_tos =  ((match->value->value.u8)<<TOS_DSCP_SHIFT_BITS ) & TOS_DSCP_BITMASK;
				masks->ip_tos |= TOS_DSCP_BITMASK;  
				num_of_matches++;
				break;
 			case OF1X_MATCH_NW_PROTO:
				matches->ip_proto = match->value->value.u8;	
				masks->ip_proto = 0xFF;
				num_of_matches++;
				break;
 			case OF1X_MATCH_NW_SRC:
				ROFL_DEBUG("of1x_entry src_ip: %d ", match->value->value.u32);
				
				matches->ip_src = htobe32( match->value->value.u32 );
				tmp_ipv4_mask = htobe32( match->value->mask.u32 );
				
				memset(&masks->ip_src,0x00,sizeof(masks->ip_src));
				if(tmp_ipv4_mask != 0xFFFF && tmp_ipv4_mask != 0x0){
					//Is wildcarded
					masks->ip_src = tmp_ipv4_mask;					 
  				}else
					num_of_matches++;
				
				break;
 			case OF1X_MATCH_NW_DST:

				ROFL_DEBUG("of1x_entry dst_ip: %d ",match->value->value.u32);
				
				matches->ip_dst = htobe32( match->value->value.u32 );
				tmp_ipv4_mask = htobe32( match->value->mask.u32 );
				
				memset(&masks->ip_dst,0x00,sizeof(masks->ip_dst));
				if(tmp_ipv4_mask != 0xFFFF && tmp_ipv4_mask != 0x0){
					//Is wildcarded
					masks->ip_dst = tmp_ipv4_mask;
					
 				}else
					num_of_matches++;
				
				break;
 			
			case OF1X_MATCH_TP_SRC:
				matches->transp_src = htobe16( match->value->value.u16 );
				masks->transp_src = 0xFFFF;
				num_of_matches++;
				break;
 			case OF1X_MATCH_TP_DST:
				matches->transp_dst = htobe16( match->value->value.u16 );	
				masks->transp_dst = 0xFFFF;
				num_of_matches++;
				break;
			
			default: //Skip
				break;

		}	
	}

	if(num_of_matches == MAX_NUM_OF_MATCHES) 
		entry->type = NETFPGA_FE_FIXED;
	else
		entry->type = NETFPGA_FE_WILDCARDED;
	
	return ROFL_SUCCESS;
}

static rofl_result_t netfpga_flow_entry_map_actions(netfpga_flow_entry_t* entry, of1x_flow_entry_t* of1x_entry){

	unsigned int i;
	uint16_t port;
	void* indirect;
	of1x_packet_action_t* action;
	netfpga_flow_entry_actions_t* actions = entry->actions;
	netfpga_align_mac_addr_t *aux;

	//If entry has not actions we are done (should we really install it down there?)
	if(!of1x_entry->inst_grp.instructions[OF1X_IT_APPLY_ACTIONS].apply_actions)
		return ROFL_SUCCESS;

	action = of1x_entry->inst_grp.instructions[OF1X_IT_APPLY_ACTIONS].apply_actions->head;
	
	if(!action){
		assert(0);
		return ROFL_FAILURE;
	}
	
	//Loop over apply actions only
	for(; action; action = action->next){
	
		//FIXME: quick hack (no aliasing in forced in all xdpd, for good reasons). Fix it!
		indirect = (void*)&action->field.u64;	

		switch(action->type){

			case OF1X_AT_SET_FIELD_ETH_DST:
				//Set auxiliary pointer
				aux = (netfpga_align_mac_addr_t*) indirect; 
				
				//inversion
				for (i=0;i<6;i++){
					actions->eth_dst.addr[i]=aux->addr[5-i];
				}
				actions->action_flags |= (1 << NETFPGA_AT_SET_DL_DST);	
				break;

			case OF1X_AT_SET_FIELD_ETH_SRC:
				//Set auxiliary pointer
				aux = (netfpga_align_mac_addr_t*) indirect; 
				
				//inversion
				for (i=0;i<6;i++){
					actions->eth_src.addr[i]=aux->addr[5-i];
				}
				actions->action_flags |= (1 << NETFPGA_AT_SET_DL_SRC);	
				break;

			case OF1X_AT_SET_FIELD_VLAN_VID:
				actions->vlan_id = htobe16((*(((uint8_t*)indirect))));
				actions->action_flags |= (1 << NETFPGA_AT_SET_VLAN_VID);	
				break;
			case OF1X_AT_SET_FIELD_VLAN_PCP:
				actions->vlan_id |= (*(((uint8_t*)indirect)) << VID_PCP_SHIFT_BITS)&VID_PCP_BITMASK;
				actions->action_flags |= (1 << NETFPGA_AT_SET_VLAN_PCP);	
				break;
			case OF1X_AT_SET_FIELD_IP_DSCP:
				actions->ip_tos = ( (*(((uint8_t*)indirect)))<<TOS_DSCP_SHIFT_BITS ) & TOS_DSCP_BITMASK;
				actions->action_flags |= (1 << NETFPGA_AT_SET_NW_TOS);	
				break;

			case OF1X_AT_SET_FIELD_NW_SRC:
				actions->ip_src = htobe32(*(((uint32_t*)indirect) ));
				actions->action_flags |= (1 << NETFPGA_AT_SET_TP_SRC);	
				break;
			case OF1X_AT_SET_FIELD_NW_DST:
				actions->ip_dst = htobe32(*(((uint32_t*)indirect) ));
				actions->action_flags |= (1 << NETFPGA_AT_SET_TP_DST);	
				break;
			case OF1X_AT_SET_FIELD_TP_SRC:
				actions->transp_src = htobe16(*(((uint16_t*)indirect) ));
				actions->action_flags |= (1 << NETFPGA_AT_SET_TP_SRC);	
				break;
			case OF1X_AT_SET_FIELD_TP_DST:
				actions->transp_dst = htobe16(*(((uint16_t*)indirect) ));
				actions->action_flags |= (1 << NETFPGA_AT_SET_TP_DST);	
				break;
			case OF1X_AT_OUTPUT:
				port = *(((uint16_t*)indirect) )&0xFFFF;
				entry->actions->action_flags=0;//added
				ROFL_DEBUG(" netfpga_flow_entry_map_actions   ENTRY port %d ",port);
				memset(&(actions->forward_bitmask),0x00,(actions->forward_bitmask));// clearing 

				if ((port >= NETFPGA_FIRST_PORT) && (port <= NETFPGA_LAST_PORT)) {
					//Send to specific port
					actions->forward_bitmask |= (0x01 << ((port - NETFPGA_PORT_BASE) * 2));
				}else if (port == NETFPGA_IN_PORT) {
					//Send back to in-port	

					ROFL_DEBUG(" \n SEND BACK TO PORT %x ",entry->matches->src_port);
					
					actions->forward_bitmask |= (entry->matches->src_port);
				}else if(port == NETFPGA_ALL_PORTS || port == NETFPGA_FLOOD_PORT) {
					ROFL_DEBUG(" \n FLOOD PORT %x ",entry->matches->src_port);
					//Send to all ports except in-port	
					for(i = NETFPGA_FIRST_PORT; i <= NETFPGA_LAST_PORT; ++i) {
						if(entry->matches->src_port != (0x1 << ((i-1) * 2))) {
							actions->forward_bitmask |= (0x1 << ((i-1) * 2));
						}
					}
				}else{
					//Wrong port!
					assert(0);
				}
				break;
			
			default:
				break;
		}
	}
	
	
	return ROFL_SUCCESS;
}

//Calculate exact position
static void netfpga_set_hw_position_exact(netfpga_device_t* nfpga, netfpga_flow_entry_t* hw_entry){
	
	uint32_t hash;
	uint32_t pos; 
	struct crc32 crc;

	crc32_init(&crc, NETFPGA_POLYNOMIAL1);
	hash = crc32_calculate(&crc, hw_entry->matches, sizeof(*hw_entry->matches));

	//Calculate the index based on the hash.  The last bits of hash is the index in the table
	pos = (NETFPGA_OPENFLOW_EXACT_TABLE_SIZE-1) & hash;

	if (nfpga->hw_exact_table[pos] == NULL) {
		hw_entry->hw_pos = pos;
		nfpga->hw_exact_table[pos] = hw_entry;
		return;
	}

	//Use fallback polynomial
	crc32_init(&crc, NETFPGA_POLYNOMIAL2);
	hash = crc32_calculate(&crc, hw_entry->matches, sizeof(*hw_entry->matches));
	
	// the bottom fixed bits of hash == the index into the table
	pos = (NETFPGA_OPENFLOW_EXACT_TABLE_SIZE-1) & hash;

	//Check existing entry
	if (nfpga->hw_exact_table[pos] != NULL) {
		//FIXME: if is the same, the previous entry should be removed... Which is the collision probability?
	}
	
	hw_entry->hw_pos = pos;
	nfpga->hw_exact_table[pos] = hw_entry;
	//ROFL_DEBUG("HW position is %d \n", pos);
}

//Determine wildcard position
/*
* FIXME: this is simply wrong. In EXACT table is correct assuming no order, since is exact, but in wildcard
* priority should be taken into account. This implies some sort of either brute force moving of entries once
* an insertion of a higher priority entries needs to happen, or better, implement priority in HW.
*
* I am just following the reference implementation here
*/
static void netfpga_set_hw_position_wildcard(netfpga_device_t* nfpga, netfpga_flow_entry_t* hw_entry){

	unsigned int i;
	
	for(i=0; i<NETFPGA_OPENFLOW_WILDCARD_TABLE_SIZE; ++i){
		if( nfpga->hw_wildcard_table[i] == NULL){
			hw_entry->hw_pos = i;
			ROFL_DEBUG(" \n Given hw_position %x ",i);
			nfpga->hw_wildcard_table[i] = hw_entry;
			return;
		}
	}		

	//This cannot happen, since pre-condition should be checked by the driver before calling rofl-pipeline entry insertion
	assert(0);
}

netfpga_flow_entry_t* netfpga_generate_hw_flow_entry(netfpga_device_t* nfpga, of1x_flow_entry_t* of1x_entry){

	netfpga_flow_entry_t* entry;

	//Create the entry container	
	entry = netfpga_init_flow_entry();
	if(!entry)
		return NULL;

	//Do the translation matches
	if(netfpga_flow_entry_map_matches(entry, of1x_entry) != ROFL_SUCCESS){
		netfpga_destroy_flow_entry(entry);
		return NULL;
	}
	
	//Do the translation actions 
	if(netfpga_flow_entry_map_actions(entry, of1x_entry) != ROFL_SUCCESS){
		netfpga_destroy_flow_entry(entry);
		return NULL;
	}

	//Determine the position of the entry
	if( entry->type == NETFPGA_FE_WILDCARDED )
		netfpga_set_hw_position_wildcard(nfpga, entry);
	else
		netfpga_set_hw_position_exact(nfpga, entry);


	return entry;	
}
