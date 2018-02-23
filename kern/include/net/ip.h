#ifndef _NET_IP_H_
#define _NET_IP_H_

#include <types.h>

// 20-60 byte header, depending on ihl field
struct generic_ipv4_hdr {
    uint8_t version: 4;
    uint8_t ihl: 4; // header length, in 4-byte words (min: 5=20bytes, max: 15=60bytes)
    uint8_t dscp: 6; // unused, used for VOIP and such
    uint8_t ecn: 2; // network congestion fields
    uint16_t total_len; // len including size of header + data
    uint16_t ident; // identification info, for use with IP datagram fragmentation
    uint8_t frag_flags: 3;
    uint16_t frag_offset: 13;
    uint8_t ttl;
    uint8_t proto;
    uint16_t header_checksum;
    uint32_t source_ip;
    uint32_t dest_ip;
    unsigned char data[]; // offset 20 bytes, the header may be longer if ihl field is > 5
} __attribute__((packed));

#endif
