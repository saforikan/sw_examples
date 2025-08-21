#include <arpa/inet.h>
#include "datagram_parser.h"

static inline
datagram_header_t parse_datagram(uint8_t *packet_hdr)
{
	datagram_header_t dg_hdr = {
		.magic = ntohs(*(uint16_t *)packet_hdr),
		.frames = ntohs(*(uint16_t *)(packet_hdr+2)),
		.sequence_number = ntohl(*(uint32_t *)(packet_hdr+4))
	};

	return dg_hdr;
}
