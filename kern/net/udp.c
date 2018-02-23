#include <generic/net.h>
#include <net/if_ether.h>
#include <net/inet.h>
#include <net/ethernet.h>
#include <net/arp.h>
#include <net/ip.h>
#include <net/udp.h>
#include <lib.h>

uint16_t udp_find_unused_port(void) {
  return 1;
}

struct udp_hdr *init_udp_hdr(uint16_t source_port, uint16_t dest_port, char *header_and_data, size_t datalen) {
  char *udp_buf = header_and_data;
  uint16_t total_len = htons(datalen + sizeof(struct udp_hdr));
  if (source_port <= 0) {
    source_port = udp_find_unused_port();
  }
  source_port = htons(source_port);
  dest_port = htons(dest_port);
  memcpy(udp_buf, &source_port, 2);
  memcpy(udp_buf+2, &dest_port, 2);
  memcpy(udp_buf+4, &total_len, 2);
  memset(udp_buf+6, 0, 2); // 0 checksum
  return (struct udp_hdr*)udp_buf;
}

struct generic_ipv4_hdr *init_ipv4_udp_hdr(uint32_t source_ip, uint32_t dest_ip, char *header_and_data, size_t datlen) {
  char *ipv4_buf = header_and_data;
  uint16_t total_len = htons(datlen + sizeof(struct generic_ipv4_hdr));
  source_ip = htons(source_ip);
  dest_ip = htons(dest_ip);
  ipv4_buf[0] = 0x45; // ipv4 with header length of 5 words
  ipv4_buf[1] = 0x00;
  memcpy(ipv4_buf+2, &total_len, 2);
  memset(ipv4_buf+4, 0, 2); // ident (unused)
  memset(ipv4_buf+6, 0, 2); // frag flags, frag offset (unused)
  memset(ipv4_buf+8, 0x00A0, 1); // ttl of 10
  memset(ipv4_buf+9, UDP_PROTO, 1);
  memset(ipv4_buf+10, 0, 2); // checksum
  memcpy(ipv4_buf+12, &source_ip, 4);
  memcpy(ipv4_buf+16, &dest_ip, 4);
  return (struct generic_ipv4_hdr*)ipv4_buf;
}

struct eth_hdr *make_udp_ipv4_packet(
  uint32_t source_ip, uint32_t dest_ip, uint16_t dest_port, char *udp_data,
  size_t udp_datalen, size_t *packlen_out
) {
  size_t packlen = sizeof(struct eth_hdr) + sizeof(struct generic_ipv4_hdr) +
    sizeof(struct udp_hdr) + udp_datalen;
  char *buf = kmalloc(packlen);
  KASSERT(buf);
  size_t udp_data_offset = packlen - udp_datalen;
  memcpy(buf+udp_data_offset, udp_data, udp_datalen);
  struct eth_hdr *eth_hdr = (struct eth_hdr*)buf;
  eth_hdr->ethertype = htons(ETH_P_IP);
  char *dest_mac = arp_lookup_mac(dest_ip);
  char *source_mac = arp_lookup_mac(source_ip);
  if (!dest_mac) {
    panic("destination macaddr unknown for IP: %u", dest_ip);
  }
  if (!source_mac) {
    source_mac = (char*)netdev->gn_macaddr;
  }
  memcpy(eth_hdr->dmac, dest_mac, 6);
  memcpy(eth_hdr->smac, source_mac, 6);
  char *ipv4_header_buf = buf + sizeof(struct eth_hdr);
  size_t ipv4_datlen = packlen - (sizeof(struct eth_hdr) + sizeof(struct generic_ipv4_hdr));
  struct generic_ipv4_hdr *ipv4_hdr = init_ipv4_udp_hdr(source_ip, dest_ip, ipv4_header_buf, ipv4_datlen);
  KASSERT(ipv4_hdr);
  char *udp_header_buf = ipv4_header_buf + sizeof(*ipv4_hdr);
  struct udp_hdr *udp_hdr = init_udp_hdr(0, dest_port, udp_header_buf, udp_datalen);
  (void)udp_hdr;
  *packlen_out = packlen;
  return eth_hdr;
}

struct udp_hdr *strip_udp_ipv4_packet(struct eth_hdr *eth_hdr) {
  KASSERT(ntohs(eth_hdr->ethertype) == ETH_P_IP);
  struct generic_ipv4_hdr *ipv4_hdr = (struct generic_ipv4_hdr*)eth_hdr->payload;
  KASSERT(ipv4_hdr);
  KASSERT(ipv4_hdr->version == 4);
  uint8_t headerlen = ipv4_hdr->ihl;
  KASSERT(headerlen == 5); // right now, it's always 5
  struct udp_hdr *udp_hdr = (struct udp_hdr*)ipv4_hdr->data;
  KASSERT(udp_hdr);
  return udp_hdr;
}
