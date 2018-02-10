#include <types.h>
#include <lib.h>
#include <proc.h>
#include <current.h>
#include <socket.h>
#include <kern/errno.h>

static bool invalid_af(int af) {
  return af != AF_UNIX && af != AF_INET;
}

static bool invalid_type(int type) {
  return type != SOCK_STREAM;
}

static bool invalid_proto(int type, int proto) {
  (void)type;
  (void)proto;
  return false;
}

struct socket *make_socket(int af, int type, int proto, int *errcode) {
  if (invalid_af(af)) {
    *errcode = EINVAL;
    return NULL;
  }
  if (invalid_type(type)) {
    *errcode = EINVAL;
    return NULL;
  }
  if (invalid_proto(type, proto)) {
    *errcode = EINVAL;
    return NULL;
  }
  struct socket *sock = kmalloc(sizeof(*sock));
  bzero(sock, sizeof(struct socket));
  KASSERT(sock);
  sock->so_af = af;
  sock->so_type = type;
  sock->so_proto = proto;
  sock->so_addr = NULL;
  sock->so_state = SS_DISCONNECTED;
  return sock;
}

struct filedes *open_socket_filedes(int af, int type, int proto, int *errcode) {
  struct socket *sock = make_socket(af, type, proto, errcode);
  if (!sock) { return NULL; }
  struct filedes *file_des = kmalloc(sizeof(struct filedes));
  KASSERT(file_des);
  bzero(file_des, sizeof(struct filedes));
  file_des->ftype = FILEDES_TYPE_SOCK;
  file_des->sock = sock;
  file_des->flags = 0;
  file_des->refcount = 1;
  file_des->lk = lock_create("socket lock");
  int fd = filetable_put(curproc, file_des, -1);
	if (fd == -1) {
		*errcode = EMFILE;
    kfree(file_des);
    kfree(sock);
		return NULL;
	}
	file_des->latest_fd = fd;
	return file_des;
}

static bool addr_in_use(const struct sockaddr *addr) {
  (void)addr;
  return false;
}

static bool valid_local_addr(const struct sockaddr *addr) {
  (void)addr;
  return true;
}

static void bind_addr(const struct sockaddr *addr) {
  (void)addr;
  // TODO: add addr to list of bound addresses
}

int bind_socket(struct socket *sock, const struct sockaddr *addr, socklen_t addrlen, int *errcode) {
  KASSERT(sock);
  KASSERT(addr);
  if (sock->so_addr != NULL || sock->so_af != addr->sa_family) {
    *errcode = EINVAL;
    return -1;
  }
  if (addr_in_use(addr)) {
    *errcode = EADDRINUSE;
    return -1;
  }
  if (addrlen != (socklen_t)addr->sa_len) {
    *errcode = EINVAL;
    return -1;
  }
  if (!valid_local_addr(addr)) {
    *errcode = EINVAL;
    return -1;
  }
  bind_addr(addr);
  sock->so_addr = addr;
  return 0;
}

int socket_accept_new_conn(struct socket *listener, struct sockaddr *addr_out, socklen_t addrlen, int *errcode) {
  KASSERT(listener);
  KASSERT(addr_out);
  (void)addrlen;
  addr_out->sa_family = AF_INET;
  addr_out->sa_len = sizeof(struct sockaddr_in);
  // simulate a connection
  strcpy(((struct sockaddr_in*)(addr_out))->sa_data, "peeraddr test");
  struct filedes *new_sock_des = open_socket_filedes(listener->so_af, listener->so_type, listener->so_proto, errcode);
  if (!new_sock_des) {
    return -1; // errcode set above
  }
  struct socket *new_sock = new_sock_des->sock;
  DEBUGASSERT(new_sock);
  new_sock->so_addr = (const struct sockaddr*)addr_out;
  new_sock->so_state = SS_CONNECTED;
  new_sock->so_peer = listener;
  return new_sock_des->latest_fd;
}

int socket_read(struct filedes *file_des, struct uio *myuio, int *errcode) {
  struct socket *sock = file_des->sock;
  if (sock->so_state != SS_CONNECTED) {
    *errcode = ENOTCONN;
    return -1;
  }
  // simulate a read
  size_t len = myuio->uio_iov->iov_len;
  char *buf = kmalloc(len);
  memset(buf, 'A', len-1);
  buf[len-1] = '\0';
  uiomove(buf, len, myuio);
  return len;
}

int socket_write(struct filedes *file_des, struct uio *myuio, int *errcode) {
  struct socket *sock = file_des->sock;
  if (sock->so_state != SS_CONNECTED) {
    *errcode = ENOTCONN;
    return -1;
  }
  // simulate write success
  size_t len = myuio->uio_iov->iov_len;
  return (int)len;
}
