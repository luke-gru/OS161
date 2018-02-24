#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <lamebus/lnet.h>
#include <platform/bus.h>
#include <net/inet.h>
#include <synch.h>
#include "autoconf.h"

int config_lnet(struct lnet_softc *lnet, int lnetno) {
	(void)lnetno;
	lnet->ln_readbuf = bus_map_area(lnet->ln_bus, lnet->ln_buspos, LNET_READBUF_OFFSET);
	lnet->ln_readbuf_pos = 0; // current position in readbuf for reading
	lnet->ln_writebuf = bus_map_area(lnet->ln_bus, lnet->ln_buspos, LNET_WRITEBUF_OFFSET);
	uint32_t status = lamebus_read_register(lnet->ln_bus, lnet->ln_buspos, LNET_STAT_REG_OFFSET);
	uint16_t hwaddr = status &~ 0xffff0000; // lower 16 bits = hwaddr
	KASSERT(hwaddr > 0);
	lnet->ln_hwaddr = hwaddr;
	spinlock_init(&lnet->ln_lock);
	lnet->ln_read = lnet_read;
	lnet->ln_write = lnet_write;
	lnet->ln_read_in_progress = false;
	lnet->ln_transmit_in_progress = false;
	lnet_clear_read_status(lnet);
	lnet_clear_write_status(lnet);
	return 0;
}

static char lnet_readbuf[LNET_READBUFSZ];
static uint16_t lnet_readbuf_segs[LNET_READBUFSZ]; // readbuf segment offsets
static uint16_t lnet_readbuf_curseg;
static size_t lnet_readbufp; // buffer pos of free data remaining

static bool is_lnet_readbuf_full() {
	return lnet_readbufp >= LNET_READBUFSZ;
}

static bool is_lnet_readbuf_nonempty() {
	return lnet_readbufp > 0;
}

void lnet_irq(/*struct lnet_softc*/ void *sc) {
	struct lnet_softc *lnet = sc;
	struct net_softc *gdev = (struct net_softc*)lnet->ln_gdev;
	DEBUG(DB_NETDEV|DB_INTERRUPT, "== LNET IRQ BEG\n");
	spinlock_acquire(&lnet->ln_lock);
	if (lnet->ln_read_in_progress || !is_lnet_readbuf_full()) {
		if (lnet_is_read_ready(lnet)) {
			// read link-level packet header (8 bytes long)
			bool can_read_more = true;
			int num_packets = 0;
			size_t bytes_copied_to_buf = 0;
			while (can_read_more) {
				unsigned char *buf = lnet->ln_readbuf + lnet->ln_readbuf_pos;
				struct link_frame linkframe;
				memcpy(&linkframe, buf, 8);
				lnet->ln_readbuf_pos += 8;
				linkframe.frame_word = ntohs(linkframe.frame_word);
				linkframe.mac_from = ntohs(linkframe.mac_from);
				linkframe.packlen = ntohs(linkframe.packlen);
				linkframe.mac_to = ntohs(linkframe.mac_to);

				if (linkframe.frame_word != LINK_FRAME_WORD || linkframe.packlen == 0) {
					DEBUG(DB_NETDEV, "  lnet (irq): read finished after %d packets\n", num_packets);
					lnet_clear_read_status(sc);
					break;
				}
				num_packets++;

				DEBUG(DB_NETDEV, "  lnet (irq) read: frame word: %d, mac_from: %d, packlen: %d, mac_to: %d\n",
					linkframe.frame_word,
					linkframe.mac_from,
					linkframe.packlen,
					linkframe.mac_to
				);

				char *inbuf;
				uint16_t readmaxlen;
				uint16_t readlen;

				if (!lnet->ln_read_in_progress) {
					inbuf = lnet_readbuf + lnet_readbufp;
					readmaxlen = LNET_READBUFSZ - (uint16_t)lnet_readbufp;
					readlen = MINVAL(linkframe.packlen-8, readmaxlen);
					lnet_readbuf_segs[lnet_readbuf_curseg++] = readlen;
					DEBUG(DB_NETDEV, "  lnet (irq) read: copying %d bytes to readbuf at buffer offset %d\n",
						(int)readlen, (int)lnet_readbufp
					);
				} else {
					inbuf = gdev->gn_readbuf;
					readmaxlen = (uint16_t)gdev->gn_readbuf_sz;
					readlen = MINVAL(linkframe.packlen-8, readmaxlen);
				}
				KASSERT(inbuf);

				memcpy(inbuf, lnet->ln_readbuf+lnet->ln_readbuf_pos, (size_t)readlen);
				bytes_copied_to_buf += (size_t)readlen;
				lnet->ln_readbuf_pos += bytes_copied_to_buf;

				if (!lnet->ln_read_in_progress) {
					lnet_readbufp += (size_t)readlen; // read buffer filled with bytes
				}

				if (lnet->ln_readbuf_pos >= LNET_READBUFSZ) {
					panic("readbuf_pos");
					lnet_clear_read_status(sc);
				}

				if (lnet->ln_read_in_progress && bytes_copied_to_buf > 0) {
					lnet->ln_read_in_progress = false;
					gdev->gn_bytes_read = bytes_copied_to_buf;
					V(gdev->gn_rsem);
					DEBUG(DB_NETDEV, "  lnet (irq): read finished after %d packets\n", num_packets);
					lnet_clear_read_status(sc);
					break;
				}
			}
		}
	}
	if (lnet->ln_transmit_in_progress) {
			if (lnet_is_transmit_complete(sc)) {
				DEBUG(DB_NETDEV, "  lnet: transmit complete\n");
				lnet_clear_write_status(sc);
				if (gdev->gn_waiting_for_write_complete) {
					V(gdev->gn_wsem);
				}
			}
	}
	spinlock_release(&lnet->ln_lock);
	DEBUG(DB_NETDEV|DB_INTERRUPT, "== LNET IRQ END\n");
}

int lnet_write(void *sc, char *buf, size_t len) {
	struct lnet_softc *lnet = sc;
	if (lnet->ln_transmit_in_progress) { return -1; }
	spinlock_acquire(&lnet->ln_lock);
	struct link_frame linkframe;
	linkframe.frame_word = htons(LINK_FRAME_WORD);
	linkframe.mac_from = htons(lnet->ln_hwaddr);
	size_t packlen = len + sizeof(linkframe);
	DEBUG(DB_NETDEV, "lnet: writing packet, length: %d\n", (int)packlen);
	linkframe.packlen = htons(packlen);
	linkframe.mac_to = htons(0xffff);
	memcpy(lnet->ln_writebuf, &linkframe, sizeof(linkframe));
	memcpy(lnet->ln_writebuf+sizeof(linkframe), buf, len);
	int res = lnet_start_transmit(sc);
	spinlock_release(&lnet->ln_lock);
	return res;
}

int lnet_read(void *sc, char *buf, size_t len) {
	struct lnet_softc *lnet = sc;
	if (lnet->ln_read_in_progress) { return -1; }
	spinlock_acquire(&lnet->ln_lock);
	if (is_lnet_readbuf_nonempty()) {
		KASSERT(lnet_readbuf_curseg > 0);
		KASSERT(lnet_readbuf_segs[0] > 0);
		size_t newlen = MINVAL(len, lnet_readbuf_segs[0]);
		DEBUG(DB_NETDEV, "lnet: copying %d bytes from readbuf into client buf\n", (int)newlen);
		memcpy(buf, lnet_readbuf, newlen);
		memmove(lnet_readbuf, lnet_readbuf + newlen, LNET_READBUFSZ - newlen);
		lnet_readbufp -= newlen;
		memmove(lnet_readbuf_segs, lnet_readbuf_segs+1, lnet_readbuf_curseg*sizeof(uint16_t));
		KASSERT(lnet_readbuf_curseg > 0);
		lnet_readbuf_curseg--;
		KASSERT(lnet_readbufp <= (4096-newlen));
		spinlock_release(&lnet->ln_lock);
		return newlen;
	} else {
		DEBUG(DB_NETDEV, "lnet: read waiting for packets (%d bytes)\n", (int)len);
		lnet->ln_read_in_progress = true;
		spinlock_release(&lnet->ln_lock);
		return 0;
	}
}

// start transmitting packets. An interrupt will occur upon completion of transmission.
int lnet_start_transmit(void *sc) {
	struct lnet_softc *lnet = sc;
	if (lnet->ln_transmit_in_progress) { return -1; }

	uint32_t ctrl = lamebus_read_register(lnet->ln_bus, lnet->ln_buspos, LNET_CTRL_REG_OFFSET);
	KASSERT((ctrl & LNET_CTRL_TRANS_IN_PROGRESS) == 0);
	ctrl |= (LNET_CTRL_TRANS_IN_PROGRESS);
	lnet->ln_transmit_in_progress = true;
	lamebus_write_register(lnet->ln_bus, lnet->ln_buspos, LNET_CTRL_REG_OFFSET, ctrl);

	return 0;
}

// NOTE: call this in the IRQ handler
bool lnet_is_transmit_complete(void *sc) {
	struct lnet_softc *lnet = sc;
	KASSERT(lnet->ln_transmit_in_progress);

	uint32_t write = lamebus_read_register(lnet->ln_bus, lnet->ln_buspos, LNET_WRIT_REG_OFFSET);
	bool is_complete = (write & LNET_WRIT_COMPLETE);
	if (is_complete) {
		lnet->ln_transmit_in_progress = false;
	}

	return is_complete;
}

// NOTE: call this in the IRQ handler (with lock held)
bool lnet_is_read_ready(void *sc) {
	struct lnet_softc *lnet = sc;
	uint32_t read = lamebus_read_register(lnet->ln_bus, lnet->ln_buspos, LNET_RECV_REG_OFFSET);
	return read & LNET_READ_READY;
}

// NOTE: call this in the IRQ handler (with lock held)
void lnet_clear_read_status(void *sc) {
	struct lnet_softc *lnet = sc;
	uint32_t read = lamebus_read_register(lnet->ln_bus, lnet->ln_buspos, LNET_RECV_REG_OFFSET);
	read &= ~(LNET_READ_READY);
	lamebus_write_register(lnet->ln_bus, lnet->ln_buspos, LNET_RECV_REG_OFFSET, read);
	lnet->ln_readbuf_pos = 0;
}

// NOTE: call this in the IRQ handler (with lock held)
void lnet_clear_write_status(void *sc) {
	struct lnet_softc *lnet = sc;
	uint32_t write = lamebus_read_register(lnet->ln_bus, lnet->ln_buspos, LNET_WRIT_REG_OFFSET);
	write &= ~(LNET_WRIT_COMPLETE);
	lamebus_write_register(lnet->ln_bus, lnet->ln_buspos, LNET_WRIT_REG_OFFSET, write);
}
