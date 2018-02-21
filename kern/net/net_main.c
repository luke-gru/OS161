#include <net/net_main.h>
#include <net/arp.h>
#include <net/if_ether.h>
#include <generic/net.h>
#include <net/udp.h>
#include <lib.h>
#include <kern/errno.h>
#include <thread.h>

static void print_hexdump(char *str, int len) {
  kprintf("Printing hexdump:\n");
  for (int i = 0; i < len; i ++) {
    if (i % 8 == 0) kprintf("\n");
    kprintf("%02x ", (unsigned char)str[i]);
  }
  kprintf("\n");
}

static void ipv4_incoming(struct eth_hdr *hdr) {
  struct generic_ipv4_hdr *ipv4_hdr = (struct generic_ipv4_hdr*)hdr->payload;
  struct udp_hdr *udp_hdr = strip_udp_ipv4_packet(hdr);
  KASSERT(udp_hdr);
  size_t datalen = udp_hdr->length - sizeof(*udp_hdr);
  if (datalen > 0) {
    char *data = (char*)udp_hdr->data; // NOTE: assumes data is NULL-terminated string
    kprintf("Received UDP data from IP: %d (%d bytes):\n  %s\n", ipv4_hdr->source_ip, datalen, data);
  } else {
    kprintf("Received UDP data of length 0 from IP %d!", ipv4_hdr->source_ip);
  }
}

static void handle_frame(struct eth_hdr *hdr) {
  switch (hdr->ethertype) {
  case ETH_P_ARP:
  case ETH_P_LOOP:
    arp_incoming(hdr);
    arp_print_translation_table();
    break;
  case ETH_P_IP:
    ipv4_incoming(hdr);
    break;
  default:
    kprintf("Unrecognized ethertype %x\n", hdr->ethertype);
    break;
  }
}

void net_main(void *argv, unsigned long argc) {
  (void)argc; (void)argv;
  if (argc < 3) {
    kprintf("net_main usage error. Needs sourceip and destip!\n");
    thread_exit(1);
  }
  uint32_t sourceip = atoi(((char**)argv)[1]);
  uint32_t destip = atoi(((char**)argv)[2]);
  if (sourceip <= 0) {
    kprintf("net_main needs sourceip and destip! Usage error.\n");
    thread_exit(1);
  }
  if (destip <= 0) {
    kprintf("net_main needs destip! Usage error.\n");
    thread_exit(1);
  }
  netdev->gn_ipaddr = sourceip;
  char buf[100];

  memset(&buf, 0, 100);

  arp_init();
  arp_bcast();

  bool sent_udp_packet = false;
  const char *udp_data = "Hello world!";
  size_t udp_datalen = strlen(udp_data)+1;

  int errcode;
  while (1) {
    if (arp_lookup_mac(destip) && !sent_udp_packet) {
      size_t packlen = 0;
      struct eth_hdr *udp_packet = make_udp_ipv4_packet(
        netdev->gn_ipaddr, destip, 1, (char*)udp_data, udp_datalen, &packlen
      );
      KASSERT(udp_packet);
      kprintf("Sending UDP packet of length: %d\n", (int)packlen);
      net_transmit(udp_packet, ETH_P_IP, packlen);
      sent_udp_packet = true;
    }
    errcode = 0;
    kprintf("net_read...\n");
    int bytes_read = 0;
    if ((bytes_read = net_read(buf, 100)) < 0) {
      kprintf("ERR: Read from net device failed with code=%d\n", errcode);
    }

    print_hexdump(buf, bytes_read);

    struct eth_hdr *hdr = init_eth_hdr(buf);

    handle_frame(hdr);
  }
}
