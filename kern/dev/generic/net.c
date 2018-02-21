#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <generic/net.h>
#include <lamebus/lnet.h>
#include <synch.h>
#include "autoconf.h"

struct net_softc *netdev;

/*
 * Machine-independent generic network device.
 */

int config_net(struct net_softc *gnet, int unit) {
	/* We use only the first network device. */
	if (unit!=0) {
		return ENODEV;
	}

	KASSERT(netdev==NULL);
	netdev = gnet;
	struct lnet_softc *ln = gnet->gn_ldev;
	KASSERT(ln != NULL);
	netdev->gn_macaddr[0] = 0x00;
	netdev->gn_macaddr[1] = 0x00;
	netdev->gn_macaddr[2] = 0x00;
	netdev->gn_macaddr[3] = 0x00;
	netdev->gn_macaddr[4] = (unsigned char)(ln->ln_hwaddr >> 8);
	netdev->gn_macaddr[5] = (unsigned char)(ln->ln_hwaddr & 0x00ff);
	netdev->gn_ipaddr = NETDEV_IP;
	struct semaphore *rsem = sem_create("network read", 0);
	KASSERT(rsem);
	struct semaphore *wsem = sem_create("network write", 0);
	KASSERT(wsem);
	netdev->gn_rsem = rsem;
	netdev->gn_wsem = wsem;
	KASSERT(netdev->gn_net_write != NULL);
	KASSERT(netdev->gn_net_read != NULL);
	netdev->gn_readbuf = NULL;
	netdev->gn_readbuf_sz = 0;
	return 0;
}

int net_transmit(struct eth_hdr *hdr, int eth_type, size_t len) {
  (void)len;
	(void)hdr; (void)eth_type;
  int res = netdev->gn_net_write(netdev->gn_ldev, (char*)hdr, len);
	if (res < 0) {
		panic("invalid result");
		return res;
	}
	P(netdev->gn_wsem); // wait until transmission complete
	kprintf("net: transmit complete!\n");
	return 0;
}

int net_read(char *buf, size_t len) {
	KASSERT(netdev->gn_readbuf == NULL);
	netdev->gn_readbuf = buf;
	netdev->gn_readbuf_sz = len;
	int res = netdev->gn_net_read(netdev->gn_ldev, buf, len);
	if (res < 0) {
		netdev->gn_readbuf = NULL;
		netdev->gn_readbuf_sz = 0;
		return res;
	}

	if (res == 0) {
		P(netdev->gn_rsem); // wait until read completes
		netdev->gn_readbuf = NULL;
		netdev->gn_readbuf_sz = 0;
		return netdev->gn_bytes_read;
	} else {
		netdev->gn_readbuf = NULL;
		netdev->gn_readbuf_sz = 0;
		return res; // read completed immediately
	}
}
