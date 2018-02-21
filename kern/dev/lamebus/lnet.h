#ifndef _LAMEBUS_LNET_H_
#define _LAMEBUS_LNET_H_

#include <spinlock.h>
#include <generic/net.h>

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
	bool ln_read_in_progress;

	/* Initialized by lower-level attachment function */
	void *ln_bus;
	uint32_t ln_buspos;

	/* Initialized by higher-level attachment function */
	void *ln_readbuf;
	size_t ln_readbuf_pos;
  void *ln_writebuf;
  uint16_t ln_hwaddr; // 16 bit hwaddr
  void *ln_gdev; // higher level (generic) net device, initialized during attachment to dev/generic/net in dev/lamebus/net_lnet.c
	int (*ln_write)(void *sc, char *buf, size_t len);
	int (*ln_read)(void *sc, char *buf, size_t len);
};

#define LINK_FRAME_WORD 0xa4b3
struct link_frame {
	uint16_t frame_word;
	uint16_t mac_from;
	uint16_t packlen;
	uint16_t mac_to;
};

/* Functions called by lower-level drivers */
void lnet_irq(/*struct lnet_softc*/ void *lnet);

/* Functions called by higher-level drivers */
int lnet_write(void *sc, char *buf, size_t len);
int lnet_read(void *sc, char *buf, size_t len);
// start transmitting packets. An interrupt will occur upon completion of transmission.
int  lnet_start_transmit(/* struct lnet_softc* */ void *lnet);
bool lnet_is_transmit_complete(void *lnet);
bool lnet_is_read_ready(void *lnet);
void lnet_clear_read_status(void *lnet);
void lnet_clear_write_status(void *lnet);

#endif /* _LAMEBUS_LNET_H_ */
