#include <net/net_main.h>
#include <net/if_ether.h>
#include <generic/net.h>
#include <net/arp.h>
#include <net/ip.h>
#include <net/udp.h>
#include <net/tcp.h>
#include <net/inet.h>
#include <lib.h>
#include <kern/errno.h>
#include <thread.h>

static struct tcp_conn *tcp_conn;

static void print_hexdump(char *str, int len) {
  kprintf("Printing hexdump (%d bytes):\n", len);
  for (int i = 0; i < len; i ++) {
    if (i % 8 == 0) kprintf("\n");
    kprintf("%02x ", (unsigned char)str[i]);
  }
  kprintf("\n");
}

// handle incoming UDP or TCP packet
static int ipv4_incoming(struct eth_hdr *hdr) {
  struct generic_ipv4_hdr *ipv4_hdr = (struct generic_ipv4_hdr*)hdr->payload;
  if (ipv4_hdr->source_ip == tcp_conn->source_ip) {
    kprintf("ipv4 incoming skipped packet due to it being sent by us\n");
    return -2; // we sent this packet
  }
  if (ipv4_hdr->proto == UDP_PROTO) {
    struct udp_hdr *udp_hdr = strip_udp_ipv4_packet(hdr);
    KASSERT(udp_hdr);
    size_t datalen = udp_hdr->length - sizeof(*udp_hdr);
    if (datalen > 0) {
      char *data = (char*)udp_hdr->data; // NOTE: assumes data is NULL-terminated string
      kprintf("Received UDP data from IP: %d (%d bytes):\n  %s\n", ipv4_hdr->source_ip, datalen, data);
    } else {
      kprintf("Received UDP data of length 0 from IP %d!", ipv4_hdr->source_ip);
    }
  } else if (ipv4_hdr->proto == TCP_PROTO) {
    struct tcp_hdr *tcp_hdr = strip_tcp_ipv4_packet(hdr);
    KASSERT(tcp_hdr);
    size_t tcp_datalen = ipv4_hdr->total_len - (sizeof(*ipv4_hdr) + sizeof(*tcp_hdr));
    if (tcp_conn->dest_ip == 0) {
      tcp_conn->dest_ip = ntohl(ipv4_hdr->source_ip);
    }
    KASSERT(tcp_conn->dest_ip == (uint32_t)ntohl(ipv4_hdr->source_ip));
    KASSERT(tcp_conn->source_ip == (uint32_t)ntohl(ipv4_hdr->dest_ip));
    size_t packlen = ipv4_hdr->total_len + sizeof(*hdr);
    int tcp_handle_res = tcp_handle_incoming(tcp_conn, hdr, tcp_hdr, packlen, tcp_datalen);
    if (tcp_handle_res != 0) {
      kprintf("Error handling incoming TCP connection\n");
      return -1;
    }
  } else {
    kprintf("Unknown IPV4 protocol, cannot handle: %d\n", ipv4_hdr->proto);
    return -1;
  }
  return 0;
}

static int handle_frame(struct eth_hdr *hdr) {
  int res;
  switch (hdr->ethertype) {
  case ETH_P_ARP:
  case ETH_P_LOOP:
    res = arp_incoming(hdr);
    if (res == 0) {
      arp_print_translation_table();
    }
    break;
  case ETH_P_IP:
    res = ipv4_incoming(hdr);
    break;
  default:
    kprintf("Unrecognized ethertype %x\n", hdr->ethertype);
    res = -1;
    break;
  }
  return res;
}

static void usage(const char *msg) {
  kprintf(msg);
  thread_exit(1);
}

// drop every other packet
static bool packetloss_simulator(char *packet) {
  if (tcp_conn->state != TCP_CONN_STATE_EST) {
    return false;
  }
  //return false;
  (void)packet;
  static unsigned int i = 0;
  i++;
  return i % 2 == 0;
}

// ex1: $ net_tcp server 255:9000
// starts TCP server connection bound to IP 255 and port 9000
// ex2: net_tcp client 256 255:9000
// sets the client's local IP to 256, and tries to establish connection to IP 255 on port 9000
void tcp_net_main(void *argv, unsigned long argc) {
  if (argc < 3) {
    usage("tcp_net_main usage error.\n");
  }
  char **args = (char**)argv;
  struct tcp_conn conn;
  uint32_t source_ip = 0;

  if (strcmp(args[1], "server") == 0) { // configure TCP server conn
    uint16_t source_port = 0;
    char *sep = strchr(args[2], ':');
    if (!sep) {
      usage("Usage error, Expected net_tcp server IP:PORT\n");
    }
    source_ip = atoi(args[2]);
    if (source_ip == 0) {
      usage("IP must be > 0!\n");
    }
    netdev->gn_ipaddr = source_ip;
    source_port = atoi(sep+1);
    if (source_port == 0) {
      usage("PORT must be > 0!\n");
    }
    init_tcp_conn_server(&conn);
    tcp_conn_server_bind(&conn, source_port);
  } else if (strcmp(args[1], "client") == 0) { // configure TCP client conn
    source_ip = atoi(args[2]);
    if (source_ip == 0) {
      usage("IP must be > 0!\n");
    }
    uint32_t dest_ip;
    uint16_t dest_port;
    char *sep = strchr(args[3], ':');
    if (!sep) {
      usage("Usage error, expected net_tcp client SOURCEIP DESTIP:DESTPORT\n");
    }
    dest_ip = atoi(args[3]);
    if (dest_ip == 0) usage("IP must be > 0!\n");
    dest_port = atoi(sep+1);
    if (dest_port == 0) usage("PORT must be > 0!\n");
    netdev->gn_ipaddr = source_ip;
    init_tcp_conn_client(&conn, dest_ip, dest_port);
    conn.flags |= TCP_CONN_FLAG_KEEPALIVE;
  } else {
    usage("net_tcp, expected client|server OPTIONS\n");
  }
  tcp_conn = &conn;

  char buf[100];
  memset(&buf, 0, 100);

  net_simulate_packet_loss_on(ETH_P_IP, TCP_PROTO, packetloss_simulator);

  // fill arp table
  arp_init();
  arp_bcast();

  const char *tcp_data_buf = "GET /file.txt";
  size_t tcp_data_buflen = strlen(tcp_data_buf)+1;
  bool tcp_data_sent = false;

  int handle_res;
  while (1) {
    if (conn.type == TCP_CONN_TYPE_CLIENT && arp_lookup_mac(conn.dest_ip) &&
        conn.state == TCP_CONN_STATE_EMPTY) {
      int res = start_tcp_handshake(&conn);
      KASSERT(res == 0);
    } else if (!tcp_data_sent && conn.type == TCP_CONN_TYPE_CLIENT && conn.state == TCP_CONN_STATE_EST) {
      kprintf("TCP client sending data to server\n");
      int res = tcp_send_data(&conn, (char*)tcp_data_buf, tcp_data_buflen);
      KASSERT(res == 0);
      tcp_data_sent = true;
    }
    kprintf("net_read...\n");
    if (conn.state == TCP_CONN_STATE_EST && conn.seq_send_una > 0) {
      kprintf("  expecting ACK-%u\n", conn.seq_send_una+1);
    }
    int bytes_read = 0;
    if ((bytes_read = net_read(buf, 100)) < 0) {
      kprintf("ERR: Read from net device failed with code=%d\n", bytes_read);
    }
    kprintf("net_read got %d bytes\n", bytes_read);

    struct eth_hdr *hdr = init_eth_hdr(buf);

    handle_res = handle_frame(hdr);
    if (handle_res == -2) { // do nothing
    } else {
      //print_hexdump(buf, bytes_read);
    }
  }
}

void udp_net_main(void *argv, unsigned long argc) {
  if (argc < 3) {
    usage("udp_net_main usage error. Needs sourceip and destip!\n");
  }
  uint32_t sourceip = atoi(((char**)argv)[1]);
  uint32_t destip = atoi(((char**)argv)[2]);
  if (sourceip <= 0) {
    usage("udp_net_main needs sourceip and destip! Usage error.\n");
  }
  if (destip <= 0) {
    usage("udp_net_main needs destip! Usage error.\n");
  }
  netdev->gn_ipaddr = sourceip;

  char buf[100];
  memset(&buf, 0, 100);

  // fill arp table
  arp_init();
  arp_bcast();

  bool sent_udp_packet = false;
  const char *udp_data = "Hello world!";
  size_t udp_datalen = strlen(udp_data)+1;

  while (1) {
    if (arp_lookup_mac(destip) && !sent_udp_packet) {
      size_t packlen = 0;
      struct eth_hdr *udp_packet = make_udp_ipv4_packet(
        netdev->gn_ipaddr, destip, 1, (char*)udp_data, udp_datalen, &packlen
      );
      KASSERT(udp_packet);
      kprintf("Sending UDP packet of length: %d\n", (int)packlen);
      net_transmit(udp_packet, ETH_P_IP, packlen);
      sent_udp_packet = true;
    }
    kprintf("net_read...\n");
    int bytes_read = 0;
    if ((bytes_read = net_read(buf, 100)) < 0) {
      kprintf("ERR: Read from net device failed with code=%d\n", bytes_read);
    }

    print_hexdump(buf, bytes_read);

    struct eth_hdr *hdr = init_eth_hdr(buf);

    handle_frame(hdr);
  }
}
