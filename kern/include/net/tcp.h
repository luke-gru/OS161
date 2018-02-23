#ifndef _NET_TCP_H_
#define _NET_TCP_H_

#include <types.h>
#include <net/if_ether.h>

#define TCP_OPTION_KIND_END 0
#define TCP_OPTION_KIND_NOP 1
#define TCP_OPTION_KIND_MSS 2
// maximum segment size option, only set in initial connection request if at all
#define TCP_MSS_BYTELEN 2

#define TCP_CTRL_URG 0x20
#define TCP_CTRL_ACK 0x10
#define TCP_CTRL_PSH 0x08
#define TCP_CTRL_RST 0x04
#define TCP_CTRL_SYN 0x02
#define TCP_CTRL_FIN 0x01

// NOTE: when changing these, make sure to change corresponding entry in
// TCP_CONN_STATE_NAMES array
#define TCP_CONN_STATE_EMPTY 0
// TCP server only
#define TCP_CONN_STATE_LISTEN 1
// TCP client only
#define TCP_CONN_STATE_SYN_SENT 2
// TCP server only
#define TCP_CONN_STATE_SYN_RECV 3
#define TCP_CONN_STATE_EST 4
#define TCP_CONN_STATE_FIN_WAIT 5
#define TCP_CONN_STATE_CLOSING 6
#define TCP_CONN_STATE_LASTACK 7
#define TCP_CONN_STATE_CLOSED 8

#define TCP_PROTO 0x06

/*

TCP Header Format
from: http://www.freesoft.org/CIE/Course/Section4/8.htm

    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |          Source Port          |       Destination Port        |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                        Sequence Number                        |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                    Acknowledgment Number                      |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |  Data |           |U|A|P|R|S|F|                               |
   | Offset| Reserved  |R|C|S|S|Y|I|            Window             |
   |       |           |G|K|H|T|N|N|                               |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |           Checksum            |         Urgent Pointer        |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                    Options                    |    Padding    |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                             data                              |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

*/

struct eth_hdr;
struct generic_ipv4_hdr;

// NOTE: variable width header (min 20 bytes), must be multiple of 4 bytes
struct tcp_hdr {
  uint16_t source_port;
  uint16_t dest_port; // off 2
  uint32_t seq_no; // seq # of first data octet in segment
  uint32_t ack_no; // next ack number we expect (if ack set)
  uint8_t data_offset: 4; // number of 32-bit words in TCP header
  uint8_t reserved: 6; // zeroed out
  uint8_t control_bits: 6; // URG/ACK/PSH/RST/SYN/FIN
  uint16_t window; // # of bytes we're willing to accept (range of sequence octets)
  uint16_t checksum;
  uint16_t urgent_ptr;
  // options: variable length (multiple of 8 bits)
  unsigned char data[];
}  __attribute__((packed));

enum tcp_conn_type {
  TCP_CONN_TYPE_CLIENT,
  TCP_CONN_TYPE_SERVER,
};
struct tcp_conn {
  enum tcp_conn_type type;
  uint32_t source_ip;
  uint32_t dest_ip;
  uint16_t source_port;
  uint16_t dest_port;
  uint32_t seq_send_una; // oldest unacknowledged sequence #
  uint32_t seq_send_next; // next sequence # to send
  uint32_t seq_recv_last; // last sequence # received from peer
  uint8_t state;
  time_t last_ack_from_peer;
};

uint32_t tcp_gen_ISN(void);
uint16_t tcp_find_unused_port(void);

char *tcp_data_begin(struct tcp_hdr *tcp);
struct eth_hdr *make_tcp_ipv4_packet(
  struct tcp_conn *conn, uint8_t tcp_opt_flags, char *tcp_data,
  size_t tcp_datalen, size_t *packlen_out
);
struct generic_ipv4_hdr *init_ipv4_tcp_hdr(uint32_t source_ip, uint32_t dest_ip, char *header_and_data, size_t datalen);
struct tcp_hdr *init_tcp_hdr(struct tcp_conn *conn, uint8_t tcp_opt_flags, char *header_and_data, size_t datalen);

/* high level TCP conn functions */
void init_tcp_conn_client(struct tcp_conn *conn, uint32_t dest_ip, uint16_t dest_port);
void init_tcp_conn_server(struct tcp_conn *conn);
int  tcp_conn_server_bind(struct tcp_conn *conn, uint16_t port);
int  start_tcp_handshake(struct tcp_conn *conn);
int  tcp_handle_incoming(struct tcp_conn *conn, struct tcp_hdr *tcp_hdr, size_t datalen);
int  tcp_send_data(struct tcp_conn *conn, char *data, size_t datalen);

/* lower level TCP conn functions */
void init_tcp_conn(struct tcp_conn *conn, enum tcp_conn_type type, uint32_t dest_ip, uint16_t dest_port);
const char *tcp_conn_state_name(uint8_t conn_state);

struct tcp_hdr *strip_tcp_ipv4_packet(struct eth_hdr*);

static inline bool tcp_is_urg_set(uint8_t control_bits) {
  return ((control_bits >> 5) & 0x01) != 0;
}
static inline bool tcp_is_ack_set(uint8_t control_bits) {
  return ((control_bits >> 4) & 0x01) != 0;
}
static inline bool tcp_is_psh_set(uint8_t control_bits) {
  return ((control_bits >> 3) & 0x01) != 0;
}
static inline bool tcp_is_rst_set(uint8_t control_bits) {
  return ((control_bits >> 2) & 0x01) != 0;
}
static inline bool tcp_is_syn_set(uint8_t control_bits) {
  return ((control_bits >> 1) & 0x01) != 0;
}
static inline bool tcp_is_fin_set(uint8_t control_bits) {
  return ((control_bits >> 0) & 0x01) != 0;
}

#endif
