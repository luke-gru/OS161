#ifndef _NET_ETHERNET_H_
#define _NET_ETHERNET_H_

#include <types.h>

// 14 byte header
struct eth_hdr {
    unsigned char dmac[6];
    unsigned char smac[6];
    uint16_t ethertype;
    unsigned char payload[]; // off 14
} __attribute__((packed));

struct eth_hdr *init_eth_hdr(char* buf);

#endif
