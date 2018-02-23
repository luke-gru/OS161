#include <generic/net.h>
#include <net/tcp.h>
#include <net/arp.h>
#include <net/inet.h>
#include <net/ip.h>
#include <lib.h>

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
const char *tcp_conn_state_name(uint8_t conn_state) {
  KASSERT(conn_state <= TCP_CONN_STATE_CLOSED);
  return TCP_CONN_STATE_NAMES[conn_state];
}

int start_tcp_handshake(struct tcp_conn *conn) {
  KASSERT(conn->state == TCP_CONN_STATE_EMPTY);
  size_t packlen = 0;
  uint8_t opt_flags = 0x00;
  opt_flags |= TCP_CTRL_SYN;
  conn->seq_send_next = tcp_gen_ISN();
  conn->seq_send_una = conn->seq_send_next;
  struct eth_hdr *eth_hdr = make_tcp_ipv4_packet(conn, opt_flags, NULL, 0, &packlen);
  KASSERT(eth_hdr);
  KASSERT(packlen > 0);
  conn->state = TCP_CONN_STATE_SYN_SENT;
  conn->seq_send_next++;
  kprintf("TCP client sending SYN to server (handshake step 1)\n");
  net_transmit(eth_hdr, ETH_P_IP, packlen);
  return 0;
}

/*
  3-way handshake:
  1) client: SYN => SYN=1
  2) server: SYN-ACK => SYN=1,ACK=2
  3) client: ACK=2

*/
int tcp_handle_incoming(struct tcp_conn *conn, struct tcp_hdr *tcp_hdr, size_t datalen) {
  const char *conn_type_str = conn->type == TCP_CONN_TYPE_CLIENT ? "client" : "server";
  if (conn->state == TCP_CONN_STATE_SYN_SENT) { // client handle SYN-ACK from server
    if (tcp_is_ack_set(tcp_hdr->control_bits)) {
      uint32_t seqno_ack = ntohl(tcp_hdr->ack_no);
      if (seqno_ack != (conn->seq_send_una+1)) {  // FIXME: until we support receiving OoO packets
        kprintf("TCP ERROR: bad peer ack no: %u, expecting %u\n", seqno_ack, conn->seq_send_una+1);
        return -1;
      }
      conn->seq_send_una = conn->seq_send_next;
    } else {
      kprintf("TCP ERROR: TCP client expected SYN-ACK (handshake step 2) [no ACK] from client\n");
      return -1;
    }
    if (tcp_is_syn_set(tcp_hdr->control_bits)) {
      uint32_t seqno_recv = ntohl(tcp_hdr->seq_no);
      KASSERT(seqno_recv > 0);
      conn->seq_recv_last = seqno_recv;
      KASSERT(datalen == 0);
    } else {
      kprintf("TCP ERROR: TCP client expected SYN-ACK (handshake step 2) [no SYN] from client\n");
      return -1;
    }
    size_t packlen = 0;
    uint8_t opt_flags = 0;
    opt_flags |= (TCP_CTRL_ACK);
    struct eth_hdr *eth_hdr = make_tcp_ipv4_packet(
      conn, opt_flags, NULL, 0, &packlen
    );
    KASSERT(eth_hdr && packlen > 0);
    conn->state = TCP_CONN_STATE_EST;
    conn->seq_send_next++;
    kprintf("TCP client established connection with server!\n");
    kprintf("TCP client sending ACK back to server (handshake step 3)\n");
    net_transmit(eth_hdr, ETH_P_IP, packlen);
  } else if (conn->state == TCP_CONN_STATE_LISTEN) { // server handle SYN from client
    if (tcp_is_syn_set(tcp_hdr->control_bits)) {
      uint32_t seqno_recv = ntohl(tcp_hdr->seq_no);
      KASSERT(seqno_recv > 0);
      conn->seq_recv_last = seqno_recv;
      KASSERT(datalen == 0);
      uint32_t my_seqno = tcp_gen_ISN();
      conn->seq_send_next = my_seqno;
      conn->seq_send_una = my_seqno;
      size_t packlen = 0;
      uint8_t opt_flags = 0;
      opt_flags |= (TCP_CTRL_ACK|TCP_CTRL_SYN);
      struct eth_hdr *eth_hdr = make_tcp_ipv4_packet(
        conn, opt_flags, NULL, 0, &packlen
      );
      KASSERT(eth_hdr && packlen > 0);
      conn->seq_send_next++;
      conn->state = TCP_CONN_STATE_SYN_RECV;
      kprintf("TCP server sending SYN-ACK (handshake step 2) to client \n");
      net_transmit(eth_hdr, ETH_P_IP, packlen);
      return 0;
    } else {
      kprintf("TCP server expected SYN, but it's not set!\n");
      return -1;
    }
  } else if (conn->state == TCP_CONN_STATE_SYN_RECV) { // server handle ACK from client
    if (tcp_is_ack_set(tcp_hdr->control_bits)) {
      uint32_t seqno_ack = ntohl(tcp_hdr->ack_no);
      if (seqno_ack != (conn->seq_send_una+1)) {  // FIXME: until we support receiving OoO packets
        kprintf("TCP ERROR: %s bad peer ack no: %u, expecting %u\n", conn_type_str, seqno_ack, conn->seq_send_una+1);
        return -1;
      }
      conn->seq_send_una = conn->seq_send_next;
      if (ntohl(tcp_hdr->seq_no) > 0) {
        conn->seq_recv_last = ntohl(tcp_hdr->seq_no);
      }
      conn->state = TCP_CONN_STATE_EST;
      KASSERT(datalen == 0);
      kprintf("TCP server established connection with client!\n");
      return 0;
    } else {
      kprintf("TCP ERROR: TCP server expected ACK (handshake step 3) from client\n");
      return -1;
    }
  } else if (conn->state == TCP_CONN_STATE_EST) { // server and client
    if (!tcp_is_ack_set(tcp_hdr->control_bits)) {
      kprintf("TCP ERROR: %s expected ACK in established connection msg\n", conn_type_str);
      return -1;
    }
    uint32_t seqno_ack = ntohl(tcp_hdr->ack_no);
    if (seqno_ack != (conn->seq_send_una)) { // FIXME: until we support receiving OoO packets
      kprintf("TCP ERROR: %s received bad peer ack no: %u, expecting %u\n", conn_type_str, seqno_ack, conn->seq_send_una);
      return -1;
    }
    conn->seq_send_una = conn->seq_send_next;
    if (ntohl(tcp_hdr->seq_no) > 0) {
      conn->seq_recv_last = ntohl(tcp_hdr->seq_no) + datalen;
    }
    if (datalen > 0) {
      kprintf("TCP %s received the following message:\n", conn_type_str);
      kprintf("  => %s\n", tcp_hdr->data); // NOTE: data expected to end in NULL byte
      tcp_send_data(conn, NULL, 0); // send an ACK in response
    } else {
      kprintf("TCP %s received an ACK:\n", conn_type_str);
    }
    return 0;
  } else {
    kprintf("Unknown connection state, cannot handle yet: %s\n", tcp_conn_state_name(conn->state));
    return -1;
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
  struct eth_hdr *eth_hdr = make_tcp_ipv4_packet(
    conn, opt_flags, data, datalen, &packlen
  );
  conn->seq_send_next += (datalen+1);
  conn->seq_send_una = conn->seq_send_next;
  net_transmit(eth_hdr, ETH_P_IP, packlen);
  return 0;
}

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
    uint32_t ackno = conn->seq_recv_last+1;
    tcp_hdr->ack_no = (uint32_t)htonl(ackno);
  } else {
    tcp_hdr->ack_no = 0;
  }
  kprintf("TCP packet: (SEQ=%d, ACK=%d, datalen=%d)\n", tcp_hdr->seq_no, tcp_hdr->ack_no, datalen);
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
  init_tcp_conn(conn, TCP_CONN_TYPE_CLIENT, dest_ip, dest_port);
}
void init_tcp_conn_server(struct tcp_conn *conn) {
  init_tcp_conn(conn, TCP_CONN_TYPE_SERVER, 0, 0);
}
int tcp_conn_server_bind(struct tcp_conn *conn, uint16_t port) {
  KASSERT(conn->type == TCP_CONN_TYPE_SERVER);
  KASSERT(conn->state == TCP_CONN_STATE_EMPTY || conn->state == TCP_CONN_STATE_CLOSED);
  conn->source_port = port;
  conn->state = TCP_CONN_STATE_LISTEN;
  return 0;
}

void init_tcp_conn(struct tcp_conn* conn, enum tcp_conn_type type, uint32_t dest_ip, uint16_t dest_port) {
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
  conn->seq_recv_last = 0;
  conn->last_ack_from_peer = 0;
  conn->state = TCP_CONN_STATE_EMPTY;
}
