#ifndef _NET_ARP_H_
#define _NET_ARP_H_

#include <types.h>
#include "net/ethernet.h"

#define ARP_ETHERNET    0x0001
#define ARP_IPV4        0x0800
#define ARP_REQUEST     0x0001
#define ARP_REPLY       0x0002

#define ARP_CACHE_LEN   32
#define ARP_FREE        0
#define ARP_WAITING     1
#define ARP_RESOLVED    2

struct arp_hdr {
    uint16_t hwtype;
    uint16_t protype; // off 2
    unsigned char hwsize;
    unsigned char prosize;
    uint16_t opcode; // off 4
    unsigned char data[];
} __attribute__((packed));

// 20 byte header
struct arp_ipv4 {
    unsigned char smac[6];
    uint32_t sip;
    unsigned char dmac[6];
    uint32_t dip;
} __attribute__((packed));

struct arp_cache_entry {
    uint16_t hwtype;
    uint32_t sip;
    unsigned char smac[6];
    unsigned int state;
};

void arp_init(void);
void arp_incoming(struct eth_hdr *hdr);
void arp_reply(struct eth_hdr *hdr, struct arp_hdr *arphdr);
void arp_bcast(void);
char *arp_lookup_mac(uint32_t ip);
void arp_print_translation_table(void);

#endif
