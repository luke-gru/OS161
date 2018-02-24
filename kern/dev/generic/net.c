#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <generic/net.h>
#include <lamebus/lnet.h>
#include <synch.h>
#include <net/ip.h>
#include <net/if_ether.h>
#include <current.h>
#include "autoconf.h"

struct net_softc *netdev;
struct net_driver_packetloss_sim *packetloss_sim;

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
	netdev->gn_waiting_for_write_complete = false;
	packetloss_sim = NULL;
	return 0;
}

void net_simulate_packet_loss_on(uint16_t ethertype, uint8_t proto, net_packetloss_sim_fn fn) {
	if (packetloss_sim != NULL) {
		kfree(packetloss_sim);
	}
	struct net_driver_packetloss_sim *sim = kmalloc(sizeof(*sim));
	sim->fn_should_drop_packet = fn;
	sim->ethertype = ethertype;
	sim->proto = proto;
	sim->is_on = true;
	packetloss_sim = sim;
}
void net_simulate_packet_loss_off(void) {
	if (packetloss_sim) {
		kfree(packetloss_sim);
		packetloss_sim = NULL;
	}
}
static bool net_should_drop_packet(struct eth_hdr *hdr, uint16_t ethertype) {
	if (!packetloss_sim || !packetloss_sim->is_on) {
		return false;
	}
	if (packetloss_sim->ethertype != 0 && packetloss_sim->ethertype != ethertype) {
		return false;
	}
	if (packetloss_sim->proto != 0) {
		if (ethertype == ETH_P_IP) {
			struct generic_ipv4_hdr *ipv4_hdr = (struct generic_ipv4_hdr*)hdr->payload;
			if (ipv4_hdr->proto != packetloss_sim->proto) {
				return false;
			}
		} else {
			// not supported
			return false;
		}
	}
	return packetloss_sim->fn_should_drop_packet((char*)hdr);
}

int net_transmit(struct eth_hdr *hdr, int eth_type, size_t len) {
	if (net_should_drop_packet(hdr, (uint16_t)eth_type)) {
		kprintf("net: PACKET DROPPED\n");
		return 0;
	}
  int res = netdev->gn_net_write(netdev->gn_ldev, (char*)hdr, len);
	if (res < 0) {
		panic("invalid result");
		return res;
	}
	if (!curthread->t_in_interrupt) {
		netdev->gn_waiting_for_write_complete = true;
		P(netdev->gn_wsem); // wait until transmission complete if we can sleep
		netdev->gn_waiting_for_write_complete = false;
	}
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
