#ifndef _SOCKET_H_
#define _SOCKET_H_

#include <kern/socket.h>
#include <types.h>

#define SOCK_STATE_NONE 0
#define SOCK_STATE_LISTEN 1
#define SOCK_STATE_CONNECTED 2

typedef struct socket socket_conn_t;

struct uio;
struct filedes;

struct socket {
  int s_af; // address family, ex: AF_INET
  int s_type; // ex: SOCK_STREAM
  int s_protof; // protocol family, ex: PF_INET
  const struct sockaddr *s_addr;
  struct socket *peer; // for connected sockets
  int s_state;
  socket_conn_t **s_conn_buf;
  size_t s_conn_bufsz;
};

struct socket *make_socket(int af, int type, int proto, int *errcode);
struct filedes *open_socket_filedes(int af, int type, int proto, int *errcode);
int bind_socket(struct socket *sock, const struct sockaddr *addr, socklen_t addrlen, int *errcode);
int socket_accept_new_conn(struct socket *sock, struct sockaddr *addr, socklen_t addrlen, int *errcode);
int socket_read(struct filedes *file_des, struct uio *myuio, int *errcode);
int socket_write(struct filedes *file_des, struct uio *myuio, int *errcode);

#endif
