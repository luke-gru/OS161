/*
 * Attachment code for having the generic network device use the LAMEbus
 * network device.
 */

#include <types.h>
#include <lib.h>
#include <generic/net.h>
#include <lamebus/lnet.h>
#include "autoconf.h"

struct net_softc *attach_net_to_lnet(int netno, struct lnet_softc *lnet) {
	struct net_softc *gnet = kmalloc(sizeof(*gnet));
	if (gnet==NULL) {
		return NULL;
	}

	(void)netno;  // unused

	gnet->gn_ldev = lnet; // attach
	gnet->gn_net_write = lnet->ln_write;
	gnet->gn_net_read = lnet->ln_read;
	KASSERT(gnet->gn_net_read && gnet->gn_net_write);
  lnet->ln_gdev = gnet;

	return gnet;
}
