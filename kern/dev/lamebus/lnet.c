/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <lamebus/lnet.h>
#include <platform/bus.h>
#include "autoconf.h"

int config_lnet(struct lnet_softc *lnet, int lnetno) {
	(void)lnetno;
	lnet->ln_readbuf_p = bus_map_area(lnet->ln_bus, lnet->ln_buspos, LNET_READBUF_OFFSET);
	lnet->ln_writebuf_p = bus_map_area(lnet->ln_bus, lnet->ln_buspos, LNET_WRITEBUF_OFFSET);
	uint32_t status = lamebus_read_register(lnet->ln_bus, lnet->ln_buspos, LNET_STAT_REG_OFFSET);
	uint16_t hwaddr = status &~ 0xffff0000; // lower 16 bits = hwaddr
	KASSERT(hwaddr > 0);
	lnet->ln_hwaddr = hwaddr;
	spinlock_init(&lnet->ln_lock);
	return 0;
}

void lnet_irq(/*struct lnet_softc*/ void *sc) {
	(void)sc;
	panic("lnet interrupt");
}

// start transmitting packets. An interrupt will occur upon completion of transmission.
int lnet_start_transmit(void *sc) {
	struct lnet_softc *lnet = sc;
	spinlock_acquire(&lnet->ln_lock);
	if (lnet->ln_transmit_in_progress) { return -1; }

	uint32_t ctrl = lamebus_read_register(lnet->ln_bus, lnet->ln_buspos, LNET_CTRL_REG_OFFSET);
	KASSERT((ctrl & LNET_CTRL_TRANS_IN_PROGRESS) == 0);
	ctrl |= (LNET_CTRL_TRANS_IN_PROGRESS);
	lamebus_write_register(lnet->ln_bus, lnet->ln_buspos, LNET_CTRL_REG_OFFSET, ctrl);
	lnet->ln_transmit_in_progress = true;

	spinlock_release(&lnet->ln_lock);
	return 0;
}

bool lnet_is_transmit_complete(void *sc) {
	struct lnet_softc *lnet = sc;
	KASSERT(lnet->ln_transmit_in_progress);
	spinlock_acquire(&lnet->ln_lock);

	uint32_t write = lamebus_read_register(lnet->ln_bus, lnet->ln_buspos, LNET_WRIT_REG_OFFSET);
	bool is_complete = (write & LNET_WRIT_COMPLETE);
	if (is_complete) {
		lnet->ln_transmit_in_progress = false;
	}

	spinlock_release(&lnet->ln_lock);
	return is_complete;
}

bool lnet_is_read_ready(void *sc) {
	struct lnet_softc *lnet = sc;
	uint32_t read = lamebus_read_register(lnet->ln_bus, lnet->ln_buspos, LNET_RECV_REG_OFFSET);
	return read & LNET_READ_READY;
}

void lnet_clear_read_status(void *sc) {
	struct lnet_softc *lnet = sc;
	uint32_t read = lamebus_read_register(lnet->ln_bus, lnet->ln_buspos, LNET_RECV_REG_OFFSET);
	read &= ~(LNET_READ_READY);
	lamebus_write_register(lnet->ln_bus, lnet->ln_buspos, LNET_RECV_REG_OFFSET, read);
}

void lnet_clear_write_status(void *sc) {
	struct lnet_softc *lnet = sc;
	uint32_t write = lamebus_read_register(lnet->ln_bus, lnet->ln_buspos, LNET_WRIT_REG_OFFSET);
	write &= ~(LNET_WRIT_COMPLETE);
	lamebus_write_register(lnet->ln_bus, lnet->ln_buspos, LNET_WRIT_REG_OFFSET, write);
}
