/*
 * mdns.c
 *
 * Copyright (C) 2016-17 - ntop.org
 *
 * This file is part of nDPI, an open source deep packet inspection
 * library based on the OpenDPI and PACE technology by ipoque GmbH
 *
 * nDPI is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * nDPI is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with nDPI.  If not, see <http://www.gnu.org/licenses/>.
 * 
 */
#include "ndpi_protocol_ids.h"

#ifdef NDPI_PROTOCOL_MDNS

#define NDPI_CURRENT_PROTO NDPI_PROTOCOL_MDNS

#include "ndpi_api.h"

#define NDPI_MAX_MDNS_REQUESTS  128

PACK_ON
struct mdns_header {
 u_int16_t transaction_id, flags, questions, answers, authority_rr, additional_rr;	
} PACK_OFF;

/**
   MDNS header is similar to dns header

   0  1  2  3  4  5  6  7  8  9  0  1  2  3  4  5
   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
   |                  ID = 0x0000                  |
   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
   |                     FLAGS                     |
   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
   |                    QDCOUNT                    |
   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
   |                    ANCOUNT                    |
   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
   |                    NSCOUNT                    |
   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
   |                    ARCOUNT                    |
   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
*/

int min_val(int a, int b){
	
	if( a <= b){
	  return a;
	}
	else
	  return b;
}

char* FlowdissectMDNS(uint8_t *payload, uint16_t payload_len,struct ndpi_flow_struct *flow) {
  uint16_t answers, i = 0;

  PACK_ON
    struct mdns_rsp_entry {
    uint16_t rsp_type, rsp_class;
    uint32_t ttl;
    uint16_t data_len;
  } PACK_OFF;

  if(((payload[2] & 0x80) != 0x80) || (payload_len < 12))
    	return; // This is a not MDNS response /

  answers = ntohs(*((uint16_t*)&payload[6]))
    + ntohs(*((uint16_t*)&payload[8]))
    + ntohs(*((uint16_t*)&payload[10]));

  payload = &payload[12], payload_len -= 12;

  while((answers > 0) && (i < payload_len)) {
    char _name[256], *name;
    struct mdns_rsp_entry rsp;
    unsigned int j;
    uint16_t rsp_type, data_len;
    uint8_t device_info = false;
    char dtype[20] = "device_unknown";
    
    memset(_name, 0, sizeof(_name));
    
    for(j=0; (i < payload_len) && (j < (sizeof(_name)-1)); i++) {
      if(payload[i] == 0x0) {
	i++;
	break;
      } else if(payload[i] < 32) {
	if(j > 0) _name[j++] = '.';
      } else if(payload[i] == 0x22) {
	_name[j++] = 'a';
	_name[j++] = 'r';
	_name[j++] = 'p';
	_name[j++] = 'a';
	i++;
	break;
      } else if(payload[i] == 0xC0) {
	uint8_t offset;
	uint16_t i_save = i;

      nested_dns_definition:
	offset = payload[i+1] - 12;
	i = offset;
	
	if((offset > i)|| (i > payload_len))
	  return; /* Invalid packet */
	else {
	  /* Pointer back */  
	  while((i < payload_len)
		&& (payload[i] != 0)
		&& (j < (sizeof(_name)-1))) {
	    if(payload[i] == 0)
	      break;
	    else if(payload[i] == 0xC0) {
	      goto nested_dns_definition;
	    } else if(payload[i] < 32) {
	     if(j > 0)	_name[j++] = '.';
	      i++;
	    } else
	      _name[j++] = payload[i++];
	  }

	  if(i_save > 0) {
	    i = i_save;
	    i_save = 0;
	  }

	  i += 2;
	  // ntop->getTrace()->traceEvent(TRACE_NORMAL, "===>>> [%d] %s", i, &payload[i-12]);
	  break;
	}
      } else
	_name[j++] = payload[i];
    }

    memcpy(&rsp, &payload[i], sizeof(rsp));
    data_len = ntohs(rsp.data_len), rsp_type = ntohs(rsp.rsp_type);

    /* Skip lenght for strings >= 32 */
    name = &_name[(data_len <= 32) ? 0 : 1];
    
#ifdef DEBUG_DISCOVERY
    ntop->getTrace()->traceEvent(TRACE_NORMAL, "===>>> [%u][%s][len=%u]", ntohs(rsp.rsp_type) & 0xFFFF, name, data_len);
#endif
    
    if(strstr(name, "._device-info._"))
      device_info = true;
    else if(strstr(name, "._airplay._") || strstr(name, "._spotify-connect._") )
      strncpy(dtype, "device_multimedia",20);
    else if(strstr(name, "_ssh._"))
      strncpy(dtype,"device_workstation",20);
    else if(strstr(name, "._daap._")
	    || strstr(name, "_afpovertcp._")
	    || strstr(name, "_adisk._")
	    || strstr(name, "_smb._")
      )
      strncpy(dtype,"device_nas",20);
    else if(strstr(name, "_hap._"))
      strncpy(dtype,"device_iot",20);
    else if(strstr(name, "_pdl-datastream._"))
      strncpy(dtype,"device_printer",20);
    
    /* 
    if((strcmp(dtype,"device_unknown")!=0) && cli_host && cli_host->getMac()) {
      Mac *m = cli_host->getMac();
      
      if(m->getDeviceType() == device_unknown)
	m->setDeviceType(dtype);
    }
    */
    
    fprintf(stdout,"device type: %s\n",dtype);

    
    switch(rsp_type) {
    case 0x1C: /* AAAA */
    case 0x01: /* AA */
    case 0x10: /* TXT */
      {
	int len = strlen(name);
	char *c;
	
	if((len > 6) && (strcmp(&name[len-6], ".local") == 0))
	  name[len-6] = 0;

	c = strstr(name, "._");
	if(c && (c != name) /* Does not begin with... */)
	  c[0] = '\0';
      }


      //if(cli_host)
	//cli_host->setName(name);
      
      if((rsp_type == 0x10 /* TXT */) && (data_len > 0)) {
	char *txt = (char*)&payload[i+sizeof(rsp)], txt_buf[256];
	uint16_t off = 0;

	while(off < data_len) {
	  uint8_t txt_len = (uint8_t)txt[off];

	  if(txt_len < data_len) {
	    txt_len = min_val(data_len-off, txt_len);

	    off++;

	    if(txt_len > 0) {
	      char *model = NULL;
	      
	      strncpy(txt_buf, &txt[off], txt_len);
	      txt_buf[txt_len] = '\0';
	      off += txt_len;

#ifdef DEBUG_DISCOVERY
	      ntop->getTrace()->traceEvent(TRACE_NORMAL, "===>>> [TXT][%s]", txt_buf);
#endif
	      
	      if(strncmp(txt_buf, "am=", 3 /* Apple Model */) == 0) model = &txt_buf[3];
	      else if(strncmp(txt_buf, "model=", 6) == 0)           model = &txt_buf[6];
	      else if(strncmp(txt_buf, "md=", 3) == 0)              model = &txt_buf[3];
	  
	      fprintf(stdout,"model: %s\n",model );
	      
	      /*
	      if(model && cli_host) {
		Mac *mac = cli_host->getMac();

		if(mac) {
		  if(device_info) {
		   // Overrite only if model is empty 
		    if(mac->getModel() == NULL)
		      mac->setModel((char*)model);
		  } else
		    mac->setModel((char*)model);
		}
	      }

	      if(strncmp(txt_buf, "nm=", 3) == 0) {
		if(cli_host) cli_host->setName(&txt_buf[3]);
	      }

	      if(strncmp(txt_buf, "ssid=", 3) == 0) {
		if(cli_host && cli_host->getMac())
		  cli_host->getMac()->setSSID(&txt_buf[5]);
	      }
	      */
	    }
	  } else
	    break;
	}
      }


      fprintf(stdout,"name: %s\n",name );
	

	  strcpy(flow->host_server_name,name);


#ifdef DEBUG_DISCOVERY
      ntop->getTrace()->traceEvent(TRACE_NORMAL, "%u) %u [%s]", answers, rsp_type, name);
#endif
      //return; /* It's enough to decode the first name */
    }

    i += sizeof(rsp) + data_len, answers--;
  }
}

static void ndpi_int_mdns_add_connection(struct ndpi_detection_module_struct
					 *ndpi_struct, struct ndpi_flow_struct *flow) {
  ndpi_set_detected_protocol(ndpi_struct, flow, NDPI_PROTOCOL_MDNS, NDPI_PROTOCOL_UNKNOWN);
}

static int ndpi_int_check_mdns_payload(struct ndpi_detection_module_struct
				       *ndpi_struct, struct ndpi_flow_struct *flow) {
  
  struct ndpi_packet_struct *packet = &flow->packet;
  struct mdns_header *h = (struct mdns_header*)packet->payload;
  u_int16_t questions = ntohs(h->questions), answers = ntohs(h->answers);
  
  //dissect mdns packet 
  FlowdissectMDNS((uint8_t*)packet->payload,packet->payload_packet_len,flow);
 

  if(((packet->payload[2] & 0x80) == 0)
     && (questions <= NDPI_MAX_MDNS_REQUESTS)
     && (answers <= NDPI_MAX_MDNS_REQUESTS)) {	
    NDPI_LOG_INFO(ndpi_struct, "found MDNS with question query\n");
    return 1;    
  }

  else if(((packet->payload[2] & 0x80) != 0)
	  && (questions == 0)
	  && (answers <= NDPI_MAX_MDNS_REQUESTS)
	  && (answers != 0)) {
    char answer[256];
    int i, j, len;

    for(i=13, j=0; (packet->payload[i] != 0) && (i < packet->payload_packet_len) && (i < (sizeof(answer)-1)); i++)
      answer[j++] = (packet->payload[i] < 13) ? '.' : packet->payload[i];
	
    answer[j] = '\0';

    /* printf("==> [%d] %s\n", j, answer);  */

    if(!ndpi_struct->disable_metadata_export) {
      len = ndpi_min(sizeof(flow->protos.mdns.answer)-1, j);
      strncpy(flow->protos.mdns.answer, (const char *)answer, len);
      flow->protos.mdns.answer[len] = '\0';
    }
    
    NDPI_LOG_INFO(ndpi_struct, "found MDNS with answer query\n");
    return 1;
  }
  
  return 0;
}

void ndpi_search_mdns(struct ndpi_detection_module_struct *ndpi_struct, struct ndpi_flow_struct *flow)
{
  struct ndpi_packet_struct *packet = &flow->packet;
  u_int16_t dport;
  
  NDPI_LOG_DBG(ndpi_struct, "search MDNS\n");

  /**
     information from http://www.it-administrator.de/lexikon/multicast-dns.html 
  */
  
  /* check if UDP packet */
  if(packet->udp != NULL) {   
    /* read destination port */
    dport = ntohs(packet->udp->dest);

    /* check standard MDNS ON port 5353 */
    if(dport == 5353 && packet->payload_packet_len >= 12) {
      /* mdns protocol must have destination address 224.0.0.251 */
	    if(packet->iph != NULL /* && ntohl(packet->iph->daddr) == 0xe00000fb */) {

	NDPI_LOG_INFO(ndpi_struct, "found MDNS with destination address 224.0.0.251 (=0xe00000fb)\n");
	
	if(ndpi_int_check_mdns_payload(ndpi_struct, flow) == 1) {
	  ndpi_int_mdns_add_connection(ndpi_struct, flow);
	  return;
	}
      }
#ifdef NDPI_DETECTION_SUPPORT_IPV6
      if(packet->iphv6 != NULL) {
	const u_int32_t *daddr = packet->iphv6->ip6_dst.u6_addr.u6_addr32;
	if(daddr[0] == htonl(0xff020000) /* && daddr[1] == 0 && daddr[2] == 0 && daddr[3] == htonl(0xfb) */) {

	  NDPI_LOG_INFO(ndpi_struct, "found MDNS with destination address ff02::fb\n");
	  
	  if(ndpi_int_check_mdns_payload(ndpi_struct, flow) == 1) {
	    ndpi_int_mdns_add_connection(ndpi_struct, flow);
	    return;
	  }
	}
      }
#endif
    }
  }
  NDPI_EXCLUDE_PROTO(ndpi_struct, flow);
}


void init_mdns_dissector(struct ndpi_detection_module_struct *ndpi_struct, u_int32_t *id, NDPI_PROTOCOL_BITMASK *detection_bitmask)
{
  ndpi_set_bitmask_protocol_detection("MDNS", ndpi_struct, detection_bitmask, *id,
				      NDPI_PROTOCOL_MDNS,
				      ndpi_search_mdns,
				      NDPI_SELECTION_BITMASK_PROTOCOL_V4_V6_UDP_WITH_PAYLOAD,
				      SAVE_DETECTION_BITMASK_AS_UNKNOWN,
				      ADD_TO_DETECTION_BITMASK);

  *id += 1;
}

#endif
