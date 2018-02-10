#include <types.h>
#include <syscall.h>
#include <proc.h>
#include <vfs.h>
#include <thread.h>
#include <current.h>
#include <uio.h>
#include <copyinout.h>
#include <addrspace.h>
#include <lib.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <socket.h>

int sys_socket(int af, int type, int proto, int *retval) {
  int errcode = 0;
  struct filedes *file_des = open_socket_filedes(af, type, proto, &errcode);
  if (!file_des) {
    DEBUG(DB_SYSCALL, "Error creating socket: %d (%s)\n", errcode, strerror(errcode));
    *retval = -1;
    return errcode;
  }
  *retval = file_des->latest_fd;
  return 0;
}

int sys_bind(int sockfd, userptr_t sockaddr_user, socklen_t sockaddr_len, int *retval) {
  if (sockfd < 0) {
    *retval = -1;
    return EBADF;
  }
  if (sockaddr_user == (userptr_t)0 || sockaddr_len <= 0) {
    *retval = -1;
    return EINVAL;
  }
  struct filedes *file_des = filetable_get(curproc, sockfd);
  struct socket *sock;
  if (!file_des || file_des->ftype != FILEDES_TYPE_SOCK) {
    *retval = -1;
    return EBADF;
  }
  sock = file_des->sock;
  KASSERT(sock);
  struct sockaddr *sockaddr = kmalloc(sockaddr_len);
  KASSERT(sockaddr);
  bzero(sockaddr, sockaddr_len);
  int copy_res = copyin(sockaddr_user, sockaddr, sockaddr_len);
  DEBUGASSERT(copy_res == 0);
  int bind_err = 0;
  int bind_res = bind_socket(sock, (const struct sockaddr*)sockaddr, sockaddr_len, &bind_err);
  if (bind_res != 0) {
    kfree(sockaddr);
    *retval = -1;
    return bind_err;
  }
  *retval = 0;
  return 0;
}

int sys_listen(int sockfd, int backlog, int *retval) {
  if (sockfd < 0) {
    *retval = -1;
    return EBADF;
  }
  struct filedes *file_des = filetable_get(curproc, sockfd);
  struct socket *sock;
  if (!file_des || file_des->ftype != FILEDES_TYPE_SOCK) {
    *retval = -1;
    return EBADF;
  }
  sock = file_des->sock;
  if (!sock->so_addr) { // call bind() first
    *retval = -1;
    return EADDRINUSE;
  }
  if (backlog <= 0) {
    *retval = -1;
    return EINVAL;
  }
  socket_conn_t **conn_buf = kmalloc(sizeof(socket_conn_t*) * backlog);
  if (!conn_buf) {
    *retval = -1;
    return ENOMEM;
  }
  sock->so_state = SS_LISTENING;
  sock->so_conn_buf = conn_buf;
  sock->so_conn_bufsz = (size_t)backlog;
  *retval = 0;
  return 0;
}

int sys_accept(int sockfd, userptr_t sockaddr_peer, userptr_t sockaddr_peer_len, int *retval) {
  if (sockfd < 0) {
    *retval = -1;
    return EBADF;
  }
  struct filedes *file_des = filetable_get(curproc, sockfd);
  struct socket *sock;
  if (!file_des || file_des->ftype != FILEDES_TYPE_SOCK) {
    *retval = -1;
    return EBADF;
  }
  sock = file_des->sock;
  if (sock->so_state != SS_LISTENING) {
    *retval = -1;
    return EINVAL;
  }
  if (sockaddr_peer == (userptr_t)0 || sockaddr_peer_len == (userptr_t)0) {
    *retval = -1;
    return EINVAL;
  }
  socklen_t addrlen = 0;
  int copy_res = copyin(sockaddr_peer_len, &addrlen, sizeof(socklen_t));
  DEBUGASSERT(copy_res == 0);
  if (addrlen <= 0 || (size_t)addrlen > sizeof(struct sockaddr_storage)) {
    *retval = -1;
    return EINVAL;
  }
  struct sockaddr *sockaddr = kmalloc(addrlen);
  KASSERT(sockaddr);
  bzero(sockaddr, addrlen);
  int accept_err = 0;
  int newfd = socket_accept_new_conn(sock, sockaddr, addrlen, &accept_err); // NOTE: might block
  if (newfd == -1) {
    *retval = -1;
    return accept_err;
  }
  copy_res = copyout(sockaddr, sockaddr_peer, addrlen);
  DEBUGASSERT(copy_res == 0);
  if ((socklen_t)sockaddr->sa_len != addrlen) {
    copy_res = copyout((socklen_t*)&sockaddr->sa_len, sockaddr_peer_len, sizeof(socklen_t));
    DEBUGASSERT(copy_res == 0);
  }
  *retval = newfd;
  return 0;
}
