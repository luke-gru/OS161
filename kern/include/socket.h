#ifndef _SOCKET_H_
#define _SOCKET_H_

#include <kern/socket.h>
#include <types.h>

#define SS_DISCONNECTED 0
#define SS_LISTENING 1
#define SS_CONNECTED 2
#define SS_CONNECTING 3
#define SS_DISCONNECTING 4

typedef struct socket socket_conn_t;

struct uio;
struct filedes;

struct socket {
  int so_af; // address family, ex: AF_INET
  int so_type; // ex: SOCK_STREAM
  int so_proto; // protocol family, ex: PF_INET
  int so_options;
  const struct sockaddr *so_addr;
  struct socket *so_peer; // for connected sockets
  int so_state;
  socket_conn_t **so_conn_buf;
  size_t so_conn_bufsz;
};

struct socket *make_socket(int af, int type, int proto, int *errcode);
struct filedes *open_socket_filedes(int af, int type, int proto, int *errcode);
int bind_socket(struct socket *sock, const struct sockaddr *addr, socklen_t addrlen, int *errcode);
int socket_accept_new_conn(struct socket *sock, struct sockaddr *addr, socklen_t addrlen, int *errcode);
int socket_read(struct filedes *file_des, struct uio *myuio, int *errcode);
int socket_write(struct filedes *file_des, struct uio *myuio, int *errcode);

#endif
