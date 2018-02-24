#ifndef _GENERIC_NET_H_
#define _GENERIC_NET_H_

#include <net/ethernet.h>

extern struct net_softc *netdev;
// For now, just one packet loss simulator can be on at once (easy to change)
extern struct net_driver_packetloss_sim *packetloss_sim;

// IP addr of 10.0.0.2 (0x10_00_00_02) = 268435458
#define NETDEV_IP (0x10000002)

typedef bool (*net_packetloss_sim_fn)(char *packet);
struct net_driver_packetloss_sim {
	net_packetloss_sim_fn fn_should_drop_packet;
	uint16_t ethertype;
	uint8_t proto; // 0 = all protocols
	bool is_on;
};

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
	bool gn_waiting_for_write_complete; // we don't wait for write completion when transmitting packets in interrupt handlers (like timer threads)
  int (*gn_net_read)(void *ldev, char *buf, size_t len);
  int (*gn_net_write)(void *ldev, char *buf, size_t len);
};

int net_transmit(struct eth_hdr *hdr, int eth_type, size_t len);
int net_read(char *buf, size_t len);
void net_simulate_packet_loss_on(uint16_t ethertype, uint8_t proto, net_packetloss_sim_fn fn);
void net_simulate_packet_loss_off(void);

#endif
