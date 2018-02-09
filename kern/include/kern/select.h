#ifndef _KERN_SELECT_H_
#define _KERN_SELECT_H_

void *memmove(void *dest, const void *src, size_t len);

#define FD_SET_MAX 10
typedef struct fd_set {
  int fd_list[FD_SET_MAX];
  unsigned fds;
} fd_set;

static inline int fd_isset(int fd, struct fd_set *fd_setp) {
  for (unsigned i = 0; i < fd_setp->fds; i++) {
    if ((fd+1) == fd_setp->fd_list[i]) {
      return 1;
    }
  }
  return 0;
}

static inline void fd_clear(int fd, struct fd_set *fd_setp) {
  for (unsigned i = 0; i < fd_setp->fds; i++) {
    if ((fd+1) == fd_setp->fd_list[i]) {
      fd_setp->fd_list[i] = 0; // redundant, but whatever
      fd_setp->fds--;
      if (fd_setp->fds > i) {
        size_t space_to_right = FD_SET_MAX-1-i;
        memmove(fd_setp->fd_list+i, fd_setp->fd_list+i+1, space_to_right);
        fd_setp->fd_list[FD_SET_MAX-1] = 0; // clear last space
      }
      break;
    }
  }
}

#define FD_ZERO(fd_setp) memset((fd_setp)->fd_list, 0, sizeof(struct fd_set))
// FIXME: don't allow more than FD_SET_MAX entries
#define FD_SET(fd, fd_setp) (fd_setp)->fd_list[(fd_setp)->fds++] = fd+1
#define FD_ISSET(fd, fd_setp) fd_isset(fd, fd_setp)
#define FD_CLR(fd, fd_setp) fd_clear(fd, fd_setp)
#define FD_SETREADY(fd, fd_setp) FD_SET(fd, fd_setp)
#define FDS_READY(fd_setp) (fd_setp)->fds

#endif
