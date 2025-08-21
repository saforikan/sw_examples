#ifndef DATAGRAM_HEADER_H
#define DATAGRAM_HEADER_H

#include <stdint.h>
#include <endian.h>

typedef struct datagram_header {
	uint16_t magic;
	uint16_t frames;
	uint32_t sequence_number;
} datagram_header_t;


typedef struct subframe_header {
	uint16_t original_len;
	uint16_t captured_len;
	uint8_t payload_specific;
	uint8_t port_num;
	uint64_t timestamp;
} subframe_header_t;

static inline
datagram_header_t parse_dg_hdr(uint8_t *packet_hdr)
{
	datagram_header_t dg_hdr = {
		.magic = be16toh(*(uint16_t *) packet_hdr),
		.frames = be16toh(*(uint16_t *) (packet_hdr+2)),
		.sequence_number = be32toh(*(uint32_t *)(packet_hdr+4))
	};

	return dg_hdr;
}

static inline
subframe_header_t parse_subframe_hdr(uint8_t *subframe_hdr)
{
	subframe_header_t sf_hdr = {
		.original_len = be16toh(*(uint16_t *)subframe_hdr),
		.captured_len = be16toh(*(uint16_t *)(subframe_hdr+2)),
		.payload_specific = be16toh(*(uint16_t *)(subframe_hdr+5)),
		.port_num = *(subframe_hdr+7),
		.timestamp  = be64toh(*(uint64_t *)(subframe_hdr+8))
	};
	return sf_hdr;
}

#endif
