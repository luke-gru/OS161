#ifndef _GENERIC_NET_H_
#define _GENERIC_NET_H_

#include <net/ethernet.h>

extern struct net_softc *netdev;
// IP addr of 10.0.0.2 (0x10_00_00_02) = 268435458
#define NETDEV_IP (0x10000002)

struct net_softc {
	/* Lower-level network device used. Initialized by lower-level attach routine. */
	void *gn_ldev;
	unsigned char gn_macaddr[6]; // NOTE: gn_macaddr[0] is the most significant byte (big-endian). The first four bytes are always 0x00
  uint32_t gn_ipaddr;
  char *gn_readbuf; // current read buffer supplied to net_read
  size_t gn_readbuf_sz; // current read buffer size supplied to net_read
  size_t gn_bytes_read;
  struct semaphore *gn_rsem; // read sem
  struct semaphore *gn_wsem; // write sem
  int (*gn_net_read)(void *ldev, char *buf, size_t len);
  int (*gn_net_write)(void *ldev, char *buf, size_t len);
};

int net_transmit(struct eth_hdr *hdr, int eth_type, size_t len);
int net_read(char *buf, size_t len);


#endif
