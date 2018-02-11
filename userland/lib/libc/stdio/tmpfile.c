#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

static int tmpfile_exists(char *fname) {
  int access_res = access(fname, F_OK);
  if (access_res == 0) {
    return 1;
  }
  if (errno == ENOENT) {
    return 0;
  } else {
    return -1; // caller must check errno for actual error
  }
}

int tmpfile(void) {
  char fname[PATH_MAX];
  const char *path_prefix = ""; // FIXME
  unsigned iters = 0;
  int exist_res = 0;
  int fd;
  do {
    if (exist_res == -1) {
      return -1; // errno is set
    }
    if (iters == 100) {
      errno = EEXIST;
      return -1;
    }
    long i = random();
    snprintf(fname, PATH_MAX-1, "%stmpfile-%ld", path_prefix, i);
    fname[PATH_MAX-1]='\0';
    iters++;
  } while ((exist_res = tmpfile_exists(fname)) != 0);

  fd = open(fname, O_RDWR|O_EXCL|O_CREAT|O_TMPFILE, 0644);
  if (fd < 0) {
    return -1; // errno set by open
  }
  return fd;
}
