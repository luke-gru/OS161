#include <net/arp.h>
#include <net/inet.h>
#include <net/ethernet.h>
#include <net/if_ether.h>
#include <generic/net.h>
#include <lib.h>

/*
 * https://tools.ietf.org/html/rfc826
 */

static struct arp_cache_entry arp_cache[ARP_CACHE_LEN];

static int insert_arp_translation_table(struct arp_hdr *hdr, struct arp_ipv4 *data) {
  struct arp_cache_entry *entry;
  for (int i = 0; i<ARP_CACHE_LEN; i++) {
      entry = &arp_cache[i];

      if (entry->state == ARP_FREE) {
          entry->state = ARP_RESOLVED;

          entry->hwtype = hdr->hwtype;
          entry->sip = data->sip;
          memcpy(entry->smac, data->smac, sizeof(entry->smac));

          return 0;
      }
  }

  return -1;
}

char *arp_lookup_mac(uint32_t ip) {
  struct arp_cache_entry *entry;

  for (int i = 0; i<ARP_CACHE_LEN; i++) {
      entry = &arp_cache[i];

      if (entry->state == ARP_FREE) continue;

      if (entry->sip == ip) {
          return (char*)entry->smac;
      }
  }
  return NULL;
}

void arp_print_translation_table(void) {
  struct arp_cache_entry *entry;
  int num_entries_found = 0;
  kprintf("ARP translation table\n");
  for (int i = 0; i<ARP_CACHE_LEN; i++) {
      entry = &arp_cache[i];
      if (entry->state == ARP_FREE) continue;
      num_entries_found++;
      char mac_buf[MAC_FMT_BUFLEN+1];
      mac_buf[MAC_FMT_BUFLEN] = '\0';
      snprintf(mac_buf, MAC_FMT_BUFLEN, MAC_FMT, entry->smac[0], entry->smac[1],
        entry->smac[2], entry->smac[3], entry->smac[4], entry->smac[5]
      );
      kprintf("  IP: %d, mac: %s\n", entry->sip, mac_buf);
  }
  if (num_entries_found == 0) {
    kprintf("  (empty)\n");
  }
}

// Update mac address for entry already in translation table, in case it changed. Returns 1 if updated
static int update_arp_translation_table(struct arp_hdr *hdr, struct arp_ipv4 *data) {
    struct arp_cache_entry *entry;
    for (int i = 0; i<ARP_CACHE_LEN; i++) {
        entry = &arp_cache[i];
        if (entry->state == ARP_FREE) continue;
        if (entry->hwtype == hdr->hwtype && entry->sip == data->sip) {
            memcpy(entry->smac, data->smac, 6);
            return 1;
        }
    }
    return 0;
}

void arp_init(void) {
    memset(arp_cache, 0, ARP_CACHE_LEN * sizeof(struct arp_cache_entry));
}

int arp_incoming(struct eth_hdr *hdr) {
    struct arp_hdr *arphdr;
    struct arp_ipv4 *arpdata;
    int merge = 0;

    arphdr = (struct arp_hdr*)hdr->payload;

    arphdr->hwtype = ntohs(arphdr->hwtype);
    arphdr->protype = ntohs(arphdr->protype);
    arphdr->opcode = ntohs(arphdr->opcode);

    if (arphdr->hwtype != ARP_ETHERNET) {
      kprintf("Unsupported HW type: %x\n", arphdr->hwtype);
      return -1;
    }

    if (arphdr->protype != ARP_IPV4) {
      kprintf("Unsupported protocol: %x\n", arphdr->protype);
      return -1;
    }

    arpdata = (struct arp_ipv4 *)arphdr->data;

    merge = update_arp_translation_table(arphdr, arpdata);

    if (!merge && insert_arp_translation_table(arphdr, arpdata) != 0) {
       panic("ERR: No free space in ARP translation table\n");
    }

    if (arpdata->sip == netdev->gn_ipaddr) {
      // we sent this packet...
      return -2;
    }

    // 0xffff_ffff is the broadcast address (for the whole network)
    if (netdev->gn_ipaddr != arpdata->dip && arpdata->dip != 0xffffffff) {
      kprintf("ARP was not for us\n");
    }

    char source_mac[6];
    memcpy(source_mac, arpdata->smac, 6);
    char mac_fmt[MAC_FMT_BUFLEN+1];
    mac_fmt[MAC_FMT_BUFLEN] = '\0';
    snprintf(mac_fmt, MAC_FMT_BUFLEN, MAC_FMT, source_mac[0], source_mac[1],
      source_mac[2], source_mac[3], source_mac[4], source_mac[5]
    );

    switch (arphdr->opcode) {
    case ARP_REQUEST:
      kprintf("got an arp request from mac=%s, ip=%d\n", mac_fmt, arpdata->sip);
      arp_reply(hdr, arphdr);
      return 0;
    case ARP_REPLY:
      kprintf("got an arp reply from mac=%s, ip=%d\n", mac_fmt, arpdata->sip);
      return 0;
    default:
      kprintf("arphdr opcode not supported: %x\n", arphdr->opcode);
      return -1;
    }
    return -1; // unreachable
}

void arp_reply(struct eth_hdr *hdr, struct arp_hdr *arphdr) {
    struct arp_ipv4 *arpdata;
    int len;
    // re-use data for new packet
    arpdata = (struct arp_ipv4*)arphdr->data;

    memcpy(arpdata->dmac, arpdata->smac, 6);
    arpdata->dip = arpdata->sip;
    memcpy(arpdata->smac, netdev->gn_macaddr, 6);
    arpdata->sip = netdev->gn_ipaddr;

    arphdr->opcode = htons(ARP_REPLY);
    arphdr->hwtype = htons(ARP_ETHERNET);
    arphdr->protype = htons(ARP_IPV4);

    len = sizeof(struct arp_hdr) + sizeof(struct arp_ipv4) + sizeof(*hdr);
    net_transmit(hdr, ETH_P_ARP, len);
}

void arp_bcast(void) {
  char dmac[6] = { 0x00, 0x00, 0x00, 0x00, 0xff, 0xff };
  char smac[6];
  memcpy(smac, netdev->gn_macaddr, 6);
  uint32_t source_ip = netdev->gn_ipaddr;
  uint32_t dest_ip = 0xffffffff;

  char *datbuf = kmalloc(100);
  memset(datbuf, 0, 100);

  uint16_t eth_p_arp = htons(ETH_P_ARP);
  // ethernet header
  memcpy(datbuf, dmac, 6);
  memcpy(datbuf+6, smac, 6);
  memcpy(datbuf+12, &eth_p_arp, 2);

  // arp header
  uint16_t arp_p_ether = htons(ARP_ETHERNET);
  uint16_t arp_ipv4 = htons(ARP_IPV4);
  uint16_t arp_req = htons(ARP_REQUEST); // opcode
  memcpy(datbuf+14, &arp_p_ether, 2);
  memcpy(datbuf+16, &arp_ipv4, 2);
  memset(datbuf+18, 0, 2); // unused
  memcpy(datbuf+20, &arp_req, 2);

  // arp IPV4 frame
  uint32_t ipv4_sip = htonl(source_ip);
  uint32_t ipv4_dip = htonl(dest_ip);
  memcpy(datbuf+22, smac, 6);
  memcpy(datbuf+28, &ipv4_sip, 4);
  memcpy(datbuf+32, dmac, 6);
  memcpy(datbuf+38, &ipv4_dip, 4);

  size_t datlen = 42;

  struct eth_hdr *eth_hdr = init_eth_hdr(datbuf);

  kprintf("transmitting %d bytes\n", (int)datlen);
  net_transmit(eth_hdr, ETH_P_ARP, datlen);
}
