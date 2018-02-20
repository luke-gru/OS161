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

#ifndef _LAMEBUS_LNET_H_
#define _LAMEBUS_LNET_H_

#include <spinlock.h>

#define LNET_READBUFSZ (4096)
#define LNET_WRITEBUFSZ (4096)
#define LNET_READBUF_OFFSET (32768)
#define LNET_WRITEBUF_OFFSET (32768+LNET_READBUFSZ)

#define LNET_RECV_REG_OFFSET 0
#define LNET_WRIT_REG_OFFSET 4
#define LNET_CTRL_REG_OFFSET 8
#define LNET_STAT_REG_OFFSET 12

#define LNET_CTRL_TRANS_IN_PROGRESS 2
#define LNET_READ_READY 1
#define LNET_WRIT_COMPLETE 1


struct lnet_softc {
	/* Initialized by config function */
	struct spinlock ln_lock;    /* protects device regs */
  bool ln_transmit_in_progress;

	/* Initialized by lower-level attachment function */
	void *ln_bus;
	uint32_t ln_buspos;

	/* Initialized by higher-level attachment function */
	void *ln_readbuf_p;
  void *ln_writebuf_p;
  uint16_t ln_hwaddr; // 16 bit hwaddr
	// void (*ls_start)(void *devdata);
	// void (*ls_input)(void *devdata, int ch);
};

/* Functions called by lower-level drivers */
void lnet_irq(/*struct lnet_softc*/ void *sc);

/* Functions called by higher-level drivers */
// start transmitting packets. An interrupt will occur upon completion of transmission.
int lnet_start_transmit(/* struct lnet_softc* */ void *lnet);
bool lnet_is_transmit_complete(void *lnet);
bool lnet_is_read_ready(void *lnet);
void lnet_clear_read_status(void *lnet);
void lnet_clear_write_status(void *lnet);

#endif /* _LAMEBUS_LSER_H_ */
