#ifndef _NET_UDP_H_
#define _NET_UDP_H_

#include <types.h>

// 2**16-1
#define UDP_NUM_PORTS (65536)
#define UDP_PROTO 0x11

struct udp_hdr {
  uint16_t source_port; // optional
  uint16_t dest_port;
  uint16_t length; // length of header + payload
  uint16_t checksum; // (optional, zeroed out if not given)
  unsigned char data[];
} __attribute__((packed));

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

uint16_t udp_find_unused_port(void);
struct udp_hdr *init_udp_hdr(uint16_t source_port, uint16_t dest_port, char *data, size_t datalen);
struct generic_ipv4_hdr *init_ipv4_udp_hdr(uint32_t source_ip, uint32_t dest_ip, char *data, size_t datlen);
struct eth_hdr *make_udp_ipv4_packet(
  uint32_t source_ip, uint32_t dest_ip, uint16_t dest_port, char *udp_data, size_t udp_datalen,
  size_t *packlen_out
);
struct udp_hdr *strip_udp_ipv4_packet(struct eth_hdr *hdr);

#endif
