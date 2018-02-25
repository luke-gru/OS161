#include <generic/net.h>
#include <net/tcp.h>
#include <net/arp.h>
#include <net/inet.h>
#include <net/ip.h>
#include <clock.h>
#include <lib.h>
#include <kern/time.h>

static const char *TCP_CONN_STATE_NAMES[] = {
  "EMPTY",
  "LISTEN",
  "SYN_SENT",
  "SYN_RECV",
  "ESTABLISHED",
  "FIN_WAIT",
  "CLOSING",
  "LASTACK",
  "CLOSED",
  NULL,
};

static const char *tcp_conn_type_name(struct tcp_conn *conn) {
  switch (conn->type) {
    case TCP_CONN_TYPE_CLIENT:
    return "client";
    case TCP_CONN_TYPE_SERVER:
    return "server";
    default:
    return "unknown?";
  }
}

static struct tcp_unacked_packet *tcp_conn_find_first_unacked_packet_after(struct tcp_conn *conn, uint32_t seqno) {
  struct tcp_unacked_packet *un_packet = conn->unacked_packets;
  while (un_packet) {
    if (un_packet->seqno > seqno) {
      return un_packet;
    }
    un_packet = un_packet->next;
  }
  return NULL;
}

static struct tcp_unacked_packet *tcp_conn_find_unacked_packet(struct tcp_conn *conn, uint32_t seqno) {
  struct tcp_unacked_packet *un_packet = conn->unacked_packets;
  while (un_packet) {
    if (un_packet->seqno == seqno) {
      return un_packet;
    }
    un_packet = un_packet->next;
  }
  return NULL;
}

static void tcp_conn_retransmit_packet(struct tcp_conn *conn, struct tcp_unacked_packet *packet, bool havelock) {
  DEBUG(DB_TCP_RETRANS, "TCP conn retransmitting packet (SEQB=%u, SEQE=%u)\n", packet->seqno, packet->seqno+packet->datalen);
  tcp_conn_transmit(conn, (struct eth_hdr*)packet->packet, packet->seqno, packet->datalen, packet->packlen, true, havelock);
}

static void tcp_conn_send_keepalive_req(struct tcp_conn *conn) {
  DEBUG(DB_TCP_CONN, "TCP %s sending keepalive request to peer after %d seconds\n", tcp_conn_type_name(conn), TCP_KEEPALIVE_TIMER_NSECS);
  tcp_send_data(conn, (char*)"", 1); // send NULL byte (datalen > 0) so peer acks us
}

static void tcp_conn_retransmit_packet_timer_cb(void *conn_generic, struct timer *timer) {
  struct tcp_conn *conn = (struct tcp_conn*)conn_generic;
  spinlock_acquire(&conn->spinlk);
  struct tcp_conn_timer *tcp_timer = conn->timers;
  struct tcp_unacked_packet *tcp_packet = NULL;
  bool timer_freed = false;
  while (tcp_timer) {
    if (tcp_timer->timer == timer) {
      KASSERT(tcp_timer->type == TCP_TIMER_TYPE_ACK);
      uint32_t seqno = tcp_timer->seqno;
      tcp_packet = tcp_conn_find_unacked_packet(conn, seqno);
      if (tcp_packet) {
        tcp_conn_retransmit_packet(conn, tcp_packet, true);
        timer_freed = true; // timer freed by above call, which calls tcp_conn_reset_ack_timer, which frees the timer...
        break;
      } else {
        panic("??");
      }
    }
    tcp_timer = tcp_timer->next;
  }
  if (!timer_freed) {
    kfree(timer);
  }
  spinlock_release(&conn->spinlk);
}

static void tcp_conn_send_keepalive_timer_cb(void *conn_generic, struct timer *timer) {
  struct tcp_conn *conn = (struct tcp_conn*)conn_generic;
  spinlock_acquire(&conn->spinlk);
  struct tcp_conn_timer *tcp_timer = conn->timers;
  while (tcp_timer) {
    if (tcp_timer->timer == timer) {
      KASSERT(tcp_timer->type == TCP_TIMER_TYPE_KEEPALIVE);
      spinlock_release(&conn->spinlk);
      tcp_conn_send_keepalive_req(conn);
      return;
    }
    tcp_timer = tcp_timer->next;
  }
  spinlock_release(&conn->spinlk);
}

static void tcp_conn_add_timer(struct tcp_conn *conn, enum tcp_timer_type type, struct timer *timer, uint32_t seqno, size_t datalen) {
  struct tcp_conn_timer *conn_timer = kmalloc(sizeof(*conn_timer));
  KASSERT(conn_timer);
  conn_timer->next = NULL;
  conn_timer->type = type;
  conn_timer->timer = timer;
  conn_timer->seqno = seqno;
  conn_timer->datalen = datalen;
  if (!conn->timers) {
    conn->timers = conn_timer;
    return;
  }
  struct tcp_conn_timer *cur = conn->timers;
  while (cur->next) { cur = cur->next; }
  cur->next = conn_timer;
}

static void tcp_conn_set_ack_timer(struct tcp_conn *conn, uint32_t seqno, size_t datalen, int nseconds) {
  KASSERT(seqno > 0);
  DEBUG(DB_TCP_RETRANS, "Setting ACK retrans timer (SEQB=%u, SEQE=%u)\n", seqno, seqno+datalen);
  struct timer *timer = clock_set_timer(TIMER_TYPE_ONCE, nseconds, tcp_conn_retransmit_packet_timer_cb, (void*)conn);
  KASSERT(timer);
  tcp_conn_add_timer(conn, TCP_TIMER_TYPE_ACK, timer, seqno, datalen);
}

static int tcp_conn_clear_timer(struct tcp_conn *conn, enum tcp_timer_type type, uint32_t seqno) {
  struct tcp_conn_timer *ttimer = conn->timers;
  struct tcp_conn_timer *ttimer_prev = NULL;
  while (ttimer) {
    if (ttimer->type == type && ttimer->seqno+ttimer->datalen == seqno) {
      clock_clear_timer(ttimer->timer);
      kfree(ttimer->timer);
      if (ttimer_prev) {
        ttimer_prev->next = ttimer->next;
      } else {
        conn->timers = ttimer->next;
      }
      kfree(ttimer);
      return 0;
    }
    ttimer_prev = ttimer;
    ttimer = ttimer->next;
  }
  return -1; // not found
}

static struct tcp_conn_timer *tcp_conn_find_timer(struct tcp_conn *conn, enum tcp_timer_type type, uint32_t seqno) {
  struct tcp_conn_timer *ttimer = conn->timers;
  while (ttimer) {
    if (ttimer->type == type && ttimer->seqno+ttimer->datalen == seqno) {
      return ttimer;
    }
    ttimer = ttimer->next;
  }
  return NULL;
}

// NOTE: seqno is last seqno for packet (expected ACK for it is seqno+1)
static void tcp_conn_reset_ack_timer(struct tcp_conn *conn, uint32_t seqno, size_t datalen, int nseconds) {
  KASSERT(nseconds > 0);
  if (tcp_conn_find_timer(conn, TCP_TIMER_TYPE_ACK, seqno+datalen)) {
    KASSERT(tcp_conn_clear_timer(conn, TCP_TIMER_TYPE_ACK, seqno+datalen) == 0);
  }
  tcp_conn_set_ack_timer(conn, seqno, datalen, nseconds);
}

static void tcp_conn_reset_keepalive_timer(struct tcp_conn *conn) {
  struct tcp_conn_timer *ttimer = NULL;
  if ((ttimer = tcp_conn_find_timer(conn, TCP_TIMER_TYPE_KEEPALIVE, 0))) {
    clock_reset_timer(ttimer->timer, TCP_KEEPALIVE_TIMER_NSECS);
  } else {
    struct timer *timer = clock_set_timer(TIMER_TYPE_CONT, TCP_KEEPALIVE_TIMER_NSECS, tcp_conn_send_keepalive_timer_cb, (void*)conn);
    KASSERT(timer);
    tcp_conn_add_timer(conn, TCP_TIMER_TYPE_KEEPALIVE, timer, 0, 0);
  }
}

// static void tcp_conn_clear_timers(struct tcp_conn *conn) {
//   struct tcp_conn_timer *timer = conn->timers;
//   struct tcp_conn_timer *timer_next;
//   while (timer) {
//     timer_next = timer->next;
//     tcp_conn_clear_ack_timer(conn, timer->seqno);
//     timer = timer_next;
//   }
// }

static uint32_t tcp_conn_lowest_una_seqno(struct tcp_conn *conn) {
  KASSERT(conn);
  struct tcp_unacked_packet *una = conn->unacked_packets;
  if (!una) {
    return 0;
  }
  uint32_t lowest = 0;
  while (una) {
    if (lowest == 0 || una->seqno < lowest) {
      lowest = una->seqno;
    }
    una = una->next;
  }
  return lowest;
}

// we free the packet and cached structure when we receive an ACK for the packet, otherwise
// we may have to re-transmit it after a timeout period
static int tcp_conn_received_ack_for_seqno(struct tcp_conn *conn, uint32_t seqno) {
  KASSERT(seqno > 0);
  spinlock_acquire(&conn->spinlk);
  DEBUG(DB_TCP_SEQ, "TCP conn received ACK: %d\n", (int)seqno+1);
  struct tcp_unacked_packet *un = conn->unacked_packets;
  struct tcp_unacked_packet *un_prev = NULL;
  conn->last_acktime_from_peer = timestamp_now();
  conn->ack_recv_latest = seqno+1;
  if (conn->seq_send_una == seqno) {
    conn->seq_send_una = tcp_conn_lowest_una_seqno(conn);
  }
  if (conn->ack_recv_max < conn->ack_recv_latest) {
    conn->ack_recv_max = conn->ack_recv_latest;
  }
  while (un) {
    if ((un->seqno+un->datalen) == seqno) {
      if (un_prev) {
        un_prev->next = un->next;
      } else {
        conn->unacked_packets = un->next;
      }
      DEBUG(DB_TCP_SEQ, "TCP conn removing kept packet (SEQB=%d,SEQE=%d) due to ACK-%u\n", (int)un->seqno, (int)un->seqno+(int)un->datalen, seqno+1);
      kfree(un->packet);
      kfree(un);
      conn->num_unacked_packets--;
      tcp_conn_clear_timer(conn, TCP_TIMER_TYPE_ACK, seqno);
      spinlock_release(&conn->spinlk);
      return 0;
    }
    un_prev = un;
    un = un->next;
  }
  tcp_conn_clear_timer(conn, TCP_TIMER_TYPE_ACK, seqno);
  spinlock_release(&conn->spinlk);
  return -1; // not found
}

static int tcp_conn_keep_unacked_packet(struct tcp_conn *conn, char *packet, size_t packlen, uint32_t seqno, size_t datalen) {
  if (conn->num_unacked_packets == TCP_MAX_UNACKED_PACKETS) {
    return -1;
  }
  struct tcp_unacked_packet *unacked = kmalloc(sizeof(*unacked));
  KASSERT(unacked);
  unacked->packet = packet;
  unacked->packlen = packlen;
  unacked->seqno = seqno;
  unacked->datalen = datalen;
  unacked->next = NULL;
  unacked->first_sent_at = timestamp_now();
  unacked->last_sent_at = unacked->first_sent_at;
  unacked->times_sent = 1;
  if (conn->unacked_packets == NULL) {
    conn->unacked_packets = unacked;
  } else {
    struct tcp_unacked_packet *un = conn->unacked_packets;
    while (un->next) { un = un->next; }
    un->next = unacked;
  }
  conn->num_unacked_packets++;
  return 0;
}

static void tcp_conn_incr_unacked_packet(struct tcp_conn *conn, struct tcp_unacked_packet *unacked) {
  (void)conn;
  KASSERT(unacked);
  unacked->times_sent++;
  unacked->last_sent_at = timestamp_now();
}

// static void tcp_conn_unlink_ooo_packet(struct tcp_conn *conn, struct tcp_recv_ooo_packet *pack) {
//   KASSERT(conn && pack);
//   struct tcp_recv_ooo_packet *cur = conn->recv_ooo_packets;
//   struct tcp_recv_ooo_packet *prev = NULL;
//   while (cur) {
//     if (cur->seqno == pack->seqno) {
//       if (prev) {
//         prev->next = cur->next;
//       } else {
//         conn->recv_ooo_packets = cur->next;
//       }
//       break;
//     }
//     prev = cur;
//     cur = cur->next;
//   }
// }

static struct tcp_recv_ooo_packet *tcp_conn_find_ooo_packet(struct tcp_conn *conn, uint32_t seqno) {
  struct tcp_recv_ooo_packet *pack = conn->recv_ooo_packets;
  while (pack) {
    if (pack->seqno == seqno) {
      return pack;
    }
    pack = pack->next;
  }
  return NULL;
}

static int tcp_conn_recv_ooo_packet(struct tcp_conn *conn, char *packet, size_t packlen, uint32_t seqno, size_t datalen, bool acked) {
  struct tcp_recv_ooo_packet *existing = tcp_conn_find_ooo_packet(conn, seqno);
  if (existing) {
    KASSERT(packlen == existing->packlen);
    existing->times_recv++;
    existing->last_recv_at = timestamp_now();
    if (!existing->acked && acked) {
      existing->acked = true;
    }
    return existing->times_recv;
  }
  if (conn->num_recv_ooo_packets == TCP_MAX_RECV_OOO_KEPT_PACKETS) {
    return -1;
  }
  struct tcp_recv_ooo_packet *pack = kmalloc(sizeof(*pack));
  KASSERT(pack);
  pack->packet = packet;
  pack->packlen = packlen;
  pack->seqno = seqno;
  pack->datalen = datalen;
  pack->first_recv_at = timestamp_now();
  pack->last_recv_at = pack->first_recv_at;
  pack->acked = acked;
  pack->times_recv = 1;
  pack->next = NULL;
  if (conn->recv_ooo_packets == NULL) {
    conn->recv_ooo_packets = pack;
  } else {
    struct tcp_recv_ooo_packet *cur = conn->recv_ooo_packets;
    while (cur->next) { cur = cur->next; }
    cur->next = pack;
  }
  conn->num_recv_ooo_packets++;
  return 1;
}

const char *tcp_conn_state_name(uint8_t conn_state) {
  KASSERT(conn_state <= TCP_CONN_STATE_CLOSED);
  return TCP_CONN_STATE_NAMES[conn_state];
}

int start_tcp_handshake(struct tcp_conn *conn) {
  KASSERT(conn->state == TCP_CONN_STATE_EMPTY);
  size_t packlen = 0;
  uint8_t opt_flags = 0x00;
  opt_flags |= TCP_CTRL_SYN;
  uint32_t seqno = tcp_gen_ISN();
  conn->seq_send_next = seqno;
  conn->seq_send_una = seqno;
  struct eth_hdr *eth_hdr = make_tcp_ipv4_packet(conn, opt_flags, NULL, 0, &packlen);
  KASSERT(eth_hdr);
  KASSERT(packlen > 0);
  conn->state = TCP_CONN_STATE_SYN_SENT;
  conn->seq_send_next++;
  DEBUG(DB_TCP_CONN, "TCP client sending SYN to server (handshake step 1)\n");
  tcp_conn_transmit(conn, eth_hdr, seqno, 0, packlen, true, false);
  return 0;
}

int tcp_conn_transmit(struct tcp_conn *conn, struct eth_hdr *eth_hdr, uint32_t seqno, size_t datalen, size_t packlen, bool expect_ack, bool havelock) {
  bool dolock = expect_ack || (conn->flags & TCP_CONN_FLAG_KEEPALIVE);
  if (dolock) {
    if (!havelock) {
      spinlock_acquire(&conn->spinlk);
    }
    if (expect_ack) {
      struct tcp_unacked_packet *unacked = NULL;
      if ((unacked = tcp_conn_find_unacked_packet(conn, seqno))) {
        tcp_conn_incr_unacked_packet(conn, unacked);
      } else {
        int keep_res = tcp_conn_keep_unacked_packet(conn, (char*)eth_hdr, packlen, seqno, datalen);
        KASSERT(keep_res == 0); // TODO: what do we do when we reached max kept packets?
      }
      tcp_conn_reset_ack_timer(conn, seqno, datalen, TCP_ACK_TIMER_NSECS);
    }
    if (conn->flags & TCP_CONN_FLAG_KEEPALIVE) {
      tcp_conn_reset_keepalive_timer(conn);
    }
    if (!havelock) {
      spinlock_release(&conn->spinlk);
    }
  }
  net_transmit(eth_hdr, ETH_P_IP, packlen);
  return 0;
}

// returns whether or not this sequence is the next expected sequence from peer
static bool tcp_conn_set_received_seq(struct tcp_conn *conn, uint32_t seqno, size_t datalen) {
  KASSERT(seqno > 0);
  uint32_t last_seqno = seqno + datalen;
  KASSERT(last_seqno >= seqno); // didn't wrap
  bool seq_in_order = conn->seq_recv_latest == 0 || seqno == conn->seq_recv_next;
  conn->seq_recv_latest = seqno;
  if (seq_in_order) {
    if ((seqno+datalen+1) > conn->seq_recv_next) {
      conn->seq_recv_next = seqno+datalen+1;
    }
  }
  return seq_in_order;
}

/*
  3-way handshake:
  1) client: SYN => SYN=1
  2) server: SYN-ACK => SYN=1,ACK=2
  3) client: ACK=2

*/
int tcp_handle_incoming(struct tcp_conn *conn, struct eth_hdr *eth_hdr_in, struct tcp_hdr *tcp_hdr_in, size_t packlen_in, size_t datalen_in) {
  const char *conn_type_str = conn->type == TCP_CONN_TYPE_CLIENT ? "client" : "server";
  struct tcp_hdr *next_tcp_pack_hdr = tcp_hdr_in;
  while (next_tcp_pack_hdr) {
    if (conn->state == TCP_CONN_STATE_SYN_SENT) { // client handle SYN-ACK from server
      if (tcp_is_ack_set(tcp_hdr_in->control_bits)) {
        uint32_t seqno_ack = ntohl(tcp_hdr_in->ack_no);
        if (seqno_ack != (conn->seq_send_una+1)) {
          DEBUG(DB_TCP_CONN, "TCP ERROR: bad peer ack no: %u, expecting %u\n", seqno_ack, conn->seq_send_una+1);
          return -1;
        }
        tcp_conn_received_ack_for_seqno(conn, seqno_ack-1);
      } else {
        DEBUG(DB_TCP_CONN, "TCP ERROR: TCP client expected SYN-ACK (handshake step 2) [no ACK] from client\n");
        return -1;
      }
      if (tcp_is_syn_set(tcp_hdr_in->control_bits)) {
        uint32_t seqno_recv = ntohl(tcp_hdr_in->seq_no);
        KASSERT(tcp_conn_set_received_seq(conn, seqno_recv, datalen_in));
        KASSERT(datalen_in == 0);
      } else {
        DEBUG(DB_TCP_CONN, "TCP ERROR: TCP client expected SYN-ACK (handshake step 2) [no SYN] from client\n");
        return -1;
      }
      size_t packlen_out = 0;
      uint8_t opt_flags = 0;
      opt_flags |= (TCP_CTRL_ACK);
      struct eth_hdr *eth_hdr_out = make_tcp_ipv4_packet(
        conn, opt_flags, NULL, 0, &packlen_out
      );
      KASSERT(eth_hdr_out && packlen_out > 0);
      conn->state = TCP_CONN_STATE_EST;
      uint32_t seqno_out = conn->seq_send_next;
      conn->seq_send_una = seqno_out;
      conn->seq_send_next++;
      DEBUG(DB_TCP_CONN, "TCP client established connection with server!\n");
      DEBUG(DB_TCP_CONN, "TCP client sending ACK back to server (handshake step 3)\n");
      tcp_conn_transmit(conn, eth_hdr_out, seqno_out, 0, packlen_out, false, false);
      return 0;
    } else if (conn->state == TCP_CONN_STATE_LISTEN) { // server handle SYN from client
      if (tcp_is_syn_set(tcp_hdr_in->control_bits)) {
        uint32_t seqno_recv = ntohl(tcp_hdr_in->seq_no);
        KASSERT(tcp_conn_set_received_seq(conn, seqno_recv, datalen_in));
        KASSERT(datalen_in == 0);
        uint32_t my_seqno_out = tcp_gen_ISN();
        conn->seq_send_next = my_seqno_out;
        conn->seq_send_una = my_seqno_out;
        size_t packlen_out = 0;
        uint8_t opt_flags = 0;
        opt_flags |= (TCP_CTRL_ACK|TCP_CTRL_SYN);
        struct eth_hdr *eth_hdr_out = make_tcp_ipv4_packet(
          conn, opt_flags, NULL, 0, &packlen_out
        );
        KASSERT(eth_hdr_out && packlen_out > 0);
        conn->seq_send_next++;
        conn->state = TCP_CONN_STATE_SYN_RECV;
        DEBUG(DB_TCP_CONN, "TCP server sending SYN-ACK (handshake step 2) to client \n");
        tcp_conn_transmit(conn, eth_hdr_out, my_seqno_out, 0, packlen_out, true, false);
        return 0;
      } else {
        DEBUG(DB_TCP_CONN, "TCP server expected SYN, but it's not set!\n");
        return -1;
      }
    } else if (conn->state == TCP_CONN_STATE_SYN_RECV) { // server handle ACK from client
      if (tcp_is_ack_set(tcp_hdr_in->control_bits)) {
        uint32_t seqno_ack = ntohl(tcp_hdr_in->ack_no);
        if (seqno_ack != (conn->seq_send_una+1)) {  // FIXME: until we support receiving OoO packets
          DEBUG(DB_TCP_CONN, "TCP ERROR: %s bad peer ack no: %u, expecting %u\n", conn_type_str, seqno_ack, conn->seq_send_una+1);
          return -1;
        }
        conn->seq_send_una = 0;
        if (ntohl(tcp_hdr_in->seq_no) > 0) {
          KASSERT(tcp_conn_set_received_seq(conn, ntohl(tcp_hdr_in->seq_no), datalen_in));
        }
        conn->state = TCP_CONN_STATE_EST;
        KASSERT(datalen_in == 0);
        DEBUG(DB_TCP_CONN, "TCP server established connection with client!\n");
        tcp_conn_received_ack_for_seqno(conn, seqno_ack-1);
        return 0;
      } else {
        DEBUG(DB_TCP_CONN, "TCP ERROR: TCP server expected ACK (handshake step 3) from client\n");
        return -1;
      }
    } else if (conn->state == TCP_CONN_STATE_EST) { // server and client
      if (!tcp_is_ack_set(tcp_hdr_in->control_bits)) {
        DEBUG(DB_TCP_CONN, "TCP ERROR: %s expected ACK in established connection msg\n", conn_type_str);
        return -1;
      }
      uint32_t seqno_ack = ntohl(tcp_hdr_in->ack_no);
      // peer acked a previous packet we sent, maybe it was dropped in the network. If we receive 2 of these we
      // re-transmit that packet immediately. Otherwise we wait for the unacked packet timer to fire.
      if (conn->seq_send_una > 0 && seqno_ack != (conn->seq_send_una+1)) {
        DEBUG(DB_TCP_SEQ, "TCP WARNING: %s received out of order peer ACK, ACK=%u, expecting ACK=%u\n", conn_type_str, seqno_ack, conn->seq_send_una+1);
        uint32_t ack_recv_latest = conn->ack_recv_latest;
        tcp_conn_received_ack_for_seqno(conn, seqno_ack-1);
        if (ack_recv_latest == seqno_ack) {
          struct tcp_unacked_packet *un_pack = tcp_conn_find_first_unacked_packet_after(conn, seqno_ack-1);
          if (un_pack) {
            DEBUG(DB_TCP_RETRANS, "TCP conn retransmitting packet due to 2 ACKs of old seqno (SEQE=%u) in a row\n", seqno_ack-1);
            tcp_conn_retransmit_packet(conn, un_pack, false);
            return 0;
          } else {
            DEBUG(DB_TCP_SEQ, "TCP ERROR: received weird ACK (ACK-%u) from peer. Ignoring...\n", seqno_ack);
            return -1;
          }
        } else {
          DEBUG(DB_TCP_RETRANS, "TCP conn waiting for UNACK timer to fire before retransmission\n");
          return 0;
        }
        return -1;
      }
      if (conn->seq_send_una > 0) {
        tcp_conn_received_ack_for_seqno(conn, seqno_ack-1);
      }
      conn->seq_send_una = conn->seq_send_next;
      uint32_t seqno_recv = ntohl(tcp_hdr_in->seq_no);
      if (seqno_recv > 0) {
        // if we receive a sequence number that is > than the next expected sequence number, we mark it as an OoO packet and ack it.
        bool next_expected_seq = tcp_conn_set_received_seq(conn, seqno_recv, datalen_in);
        if (!next_expected_seq && seqno_recv > conn->seq_recv_next && datalen_in > 0) {
          tcp_conn_recv_ooo_packet(conn, (char*)eth_hdr_in, packlen_in, seqno_recv, datalen_in, true);
          // TODO: ack it
        }
      }
      if (datalen_in > 0) {
        DEBUG(DB_TCP_DAT, "TCP %s received the following message:\n", conn_type_str);
        DEBUG(DB_TCP_DAT, "  => %s\n", tcp_hdr_in->data); // NOTE: data expected to end in NULL byte
        tcp_send_data(conn, NULL, 0); // send an ACK in response
      } else {
        DEBUG(DB_TCP_SEQ, "TCP %s received ACK-%u:\n", conn_type_str, seqno_ack);
      }
      return 0;
    } else {
      DEBUG(DB_TCP_ERR, "Unknown connection state, cannot handle yet: %s\n", tcp_conn_state_name(conn->state));
      return -1;
    }
  }
  return 0;
}

// Regular data transmission for established TCP connection
int tcp_send_data(struct tcp_conn *conn, char *data, size_t datalen) {
  if (conn->state != TCP_CONN_STATE_EST) {
    return -1;
  }
  size_t packlen;
  uint8_t opt_flags = TCP_CTRL_ACK;
  uint32_t seqno = conn->seq_send_next;
  struct eth_hdr *eth_hdr = make_tcp_ipv4_packet(
    conn, opt_flags, data, datalen, &packlen
  );
  conn->seq_send_next += (datalen+1);
  bool expect_ack = datalen > 0;
  if (expect_ack) {
    conn->seq_send_una = conn->seq_send_next-1; // FIXME: should be lowest una seqno
  } else {
    conn->seq_send_una = tcp_conn_lowest_una_seqno(conn);
  }
  return tcp_conn_transmit(conn, eth_hdr, seqno, datalen, packlen, expect_ack, false);
}

// NOTE: conn->seq_send_next and conn->seq_recv_max+1 are used for SEQ and ACK fields, respectively
struct eth_hdr *make_tcp_ipv4_packet(
  struct tcp_conn *conn, uint8_t tcp_opt_flags, char *tcp_data,
  size_t tcp_datalen, size_t *packlen_out
) {
  size_t packlen = sizeof(struct eth_hdr) + sizeof(struct generic_ipv4_hdr) +
    sizeof(struct tcp_hdr) + tcp_datalen;
  char *buf = kmalloc(packlen);
  KASSERT(buf);
  memset(buf, 0, packlen);
  if (tcp_datalen > 0) {
    memcpy(buf+packlen-tcp_datalen, tcp_data, tcp_datalen);
  }
  struct eth_hdr *eth_hdr = (struct eth_hdr*)buf;
  eth_hdr->ethertype = htons(ETH_P_IP);
  char *dest_mac = arp_lookup_mac(conn->dest_ip);
  char *source_mac = arp_lookup_mac(conn->source_ip);
  if (!dest_mac) {
    panic("destination macaddr unknown for IP: %u", conn->dest_ip);
  }
  if (!source_mac) {
    source_mac = (char*)netdev->gn_macaddr;
  }
  memcpy(eth_hdr->dmac, dest_mac, 6);
  memcpy(eth_hdr->smac, source_mac, 6);
  char *ipv4_header_buf = buf + sizeof(struct eth_hdr);
  size_t ipv4_datlen = packlen - (sizeof(struct eth_hdr) + sizeof(struct generic_ipv4_hdr));
  struct generic_ipv4_hdr *ipv4_hdr = init_ipv4_tcp_hdr(conn->source_ip, conn->dest_ip, ipv4_header_buf, ipv4_datlen);
  KASSERT(ipv4_hdr);
  char *tcp_header_buf = ipv4_header_buf + sizeof(struct generic_ipv4_hdr);
  struct tcp_hdr *tcp_hdr = init_tcp_hdr(conn, tcp_opt_flags, tcp_header_buf, tcp_datalen);
  (void)tcp_hdr;
  *packlen_out = packlen;
  return eth_hdr;
}

struct generic_ipv4_hdr *init_ipv4_tcp_hdr(uint32_t source_ip, uint32_t dest_ip, char *header_and_data, size_t datlen) {
  char *ipv4_buf = header_and_data;
  uint16_t total_len = htons(datlen + sizeof(struct generic_ipv4_hdr));
  source_ip = htons(source_ip);
  dest_ip = htons(dest_ip);
  ipv4_buf[0] = 0x45; // ipv4 with header length of 5 words
  ipv4_buf[1] = 0x00;
  memcpy(ipv4_buf+2, &total_len, 2);
  memset(ipv4_buf+4, 0, 2); // ident (unused)
  memset(ipv4_buf+6, 0, 2); // frag flags, frag offset (unused)
  memset(ipv4_buf+8, 0x00A0, 1); // ttl of 10
  memset(ipv4_buf+9, TCP_PROTO, 1);
  memset(ipv4_buf+10, 0, 2); // checksum
  memcpy(ipv4_buf+12, &source_ip, 4);
  memcpy(ipv4_buf+16, &dest_ip, 4);
  return (struct generic_ipv4_hdr*)ipv4_buf;
}

struct tcp_hdr *init_tcp_hdr(struct tcp_conn *conn, uint8_t tcp_opt_flags, char *header_and_data, size_t datalen) {
  (void)datalen;
  struct tcp_hdr *tcp_hdr = (struct tcp_hdr*)header_and_data;
  KASSERT(tcp_hdr);
  tcp_hdr->source_port = htons(conn->source_port);
  tcp_hdr->dest_port = htons(conn->dest_port);
  tcp_hdr->seq_no = conn->seq_send_next;
  KASSERT(tcp_hdr->seq_no > 0);
  if (tcp_opt_flags & TCP_CTRL_ACK) {
    uint32_t ackno = conn->seq_recv_next;
    tcp_hdr->ack_no = (uint32_t)htonl(ackno);
  } else {
    tcp_hdr->ack_no = 0;
  }
  DEBUG(DB_TCP_SEQ, "TCP packet: (SEQB=%d, ACK=%d, datalen=%d)\n", tcp_hdr->seq_no, tcp_hdr->ack_no, datalen);
  tcp_hdr->data_offset = 5; // 20-byte header (5 4-byte words)
  tcp_hdr->reserved = 0;
  tcp_hdr->control_bits = tcp_opt_flags;
  tcp_hdr->window = 0; // TODO
  tcp_hdr->checksum = 0; // TODO
  tcp_hdr->urgent_ptr = 0; // TODO

  return tcp_hdr;
}

struct tcp_hdr *strip_tcp_ipv4_packet(struct eth_hdr *eth_hdr) {
  KASSERT(ntohs(eth_hdr->ethertype) == ETH_P_IP);
  struct generic_ipv4_hdr *ipv4_hdr = (struct generic_ipv4_hdr*)eth_hdr->payload;
  KASSERT(ipv4_hdr);
  KASSERT(ipv4_hdr->version == 4);
  uint8_t headerlen = ipv4_hdr->ihl;
  KASSERT(headerlen == 5); // right now, it's always 5
  struct tcp_hdr *tcp_hdr = (struct tcp_hdr*)ipv4_hdr->data;
  KASSERT(tcp_hdr);
  return tcp_hdr;
}

char *tcp_data_begin(struct tcp_hdr *tcp) {
  uint8_t word_offset = tcp->data_offset & (~0xf0);
  KASSERT(word_offset >= 5);
  KASSERT(word_offset % 4 == 0);
  size_t byte_offset = word_offset * 4;
  char *buf = (char*)tcp;
  return buf + byte_offset;
}

// TODO
uint32_t tcp_gen_ISN(void) {
  return 1;
}

// TODO
uint16_t tcp_find_unused_port(void) {
  return 1;
}

void init_tcp_conn_client(struct tcp_conn *conn, uint32_t dest_ip, uint16_t dest_port) {
  init_tcp_conn(conn, TCP_CONN_TYPE_CLIENT, dest_ip, dest_port, 0);
}
void init_tcp_conn_server(struct tcp_conn *conn) {
  init_tcp_conn(conn, TCP_CONN_TYPE_SERVER, 0, 0, 0);
}
int tcp_conn_server_bind(struct tcp_conn *conn, uint16_t port) {
  KASSERT(conn->type == TCP_CONN_TYPE_SERVER);
  KASSERT(conn->state == TCP_CONN_STATE_EMPTY || conn->state == TCP_CONN_STATE_CLOSED);
  conn->source_port = port;
  conn->state = TCP_CONN_STATE_LISTEN;
  return 0;
}

void init_tcp_conn(struct tcp_conn* conn, enum tcp_conn_type type, uint32_t dest_ip, uint16_t dest_port, int flags) {
  conn->type = type;
  conn->source_ip = netdev->gn_ipaddr;
  if (type == TCP_CONN_TYPE_CLIENT) {
    conn->source_port = tcp_find_unused_port();
    KASSERT(dest_ip > 0);
    KASSERT(dest_port > 0);
    conn->dest_ip = dest_ip;
    conn->dest_port = dest_port;
  } else {
    conn->source_port = 0; // empty until tcp_conn_server_bind() is called
    KASSERT(dest_ip == 0);
    KASSERT(dest_port == 0);
    conn->dest_ip = 0;
    conn->dest_port = 0;
  }
  conn->seq_send_una = 0;
  conn->seq_send_next = 0;
  conn->seq_recv_latest = 0;
  conn->seq_recv_next = 0;
  conn->ack_recv_latest = 0;
  conn->ack_recv_max = 0;
  conn->rtt_est_start = 0;
  conn->rtt_est = 0;
  conn->state = TCP_CONN_STATE_EMPTY;
  conn->flags = flags;
  conn->unacked_packets = NULL;
  conn->num_unacked_packets = 0;
  conn->timers = NULL;
  conn->recv_ooo_packets = NULL;
  conn->num_recv_ooo_packets = 0;
  spinlock_init(&conn->spinlk);
}
