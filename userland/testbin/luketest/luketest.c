#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <sys/stat.h>
#include <errno.h>

static void fcntl_test(int argc, char **argv) {
  if (argc == 3) { // luketest fcntl existingfile.txt
    int fd = open(argv[2], O_RDONLY, 0);
    if (fd < 3) {
      errx(1, "Unable to open file %s\n", argv[2]);
    }
    int res = fcntl(fd, F_SETFD, O_CLOEXEC);
    if (res != 0) {
      errx(1, "Unable to SETFD CLOEXEC the file\n");
    }
    char *args[4];
    for (int i = 0; i < 4; i++) {
      if (i < 2) {
        args[i] = argv[i];
      } else if (i == 2) {
        char fd_string[3]; memset(fd_string, 0, 3);
        snprintf(fd_string, 3, "%d", fd);
        args[i] = fd_string;
      } else if (i == 3) {
        args[i] = (char*)"INEXEC";
      }
    }
    execv(argv[0], args);
  } else if (argc == 4) { // luketest fcntl FD INEXEC (called from above case)
    struct stat st;
    int fd = atoi(argv[2]);
    if (fd < 3) {
      errx(1, "Invalid fd given to luketest fcntl FD INEXEC\n");
    }
    int fstat_res = fstat(fd, &st);
    if (fstat_res != -1) {
      errx(1, "Should not allow fstat for fd %d: FD should not be open after exec\n", fd);
    }
  } else {
    errx(1, "Usage: luketest fcntl MYFILE (INEXEC)\n");
  }
}

static void pipe_test(int argc, char **argv) {
  (void)argc; (void)argv;
  int pipefd[2];
  pid_t cpid;
  char buf;

  if (pipe(pipefd, 1) == -1) {
    errx(1, "pipe() failed");
  }

  if (pipefd[0] < 3) {
    errx(1, "Failed to get reader FD");
  }

  if (pipefd[1] < 3) {
    errx(1, "Failed to get writer FD");
  }

  printf("reader: %d, writer: %d\n", pipefd[0], pipefd[1]);

  cpid = fork();
  if (cpid == -1) {
    errx(1, "fork failed");
  }

  if (cpid == 0) {    /* Child reads from pipe */
    close(pipefd[1]);          /* Close unused write end */

    while (read(pipefd[0], &buf, 1) > 0)
      write(1, &buf, 1);

    write(1, "\n", 1);
    close(pipefd[0]);
    _exit(0);

  } else {            /* Parent writes argv[1] to pipe */
    close(pipefd[0]);          /* Close unused read end */
    write(pipefd[1], argv[1], strlen(argv[1]));
    close(pipefd[1]);          /* Reader will see EOF */
    int exitstatus;
    waitpid(cpid, &exitstatus, 0);              /* Wait for child */
  }
}

static void files_test(int argc, char **argv) {
  if (argc != 3) {
    errx(1, "must give file to write to");
  }
  int fd = open(argv[2], O_RDONLY, 0644);
  if (fd < 3) {
    errx(1, "Error opening file: %s", argv[2]);
  }
  struct stat *st = malloc(sizeof(struct stat));
  int page_res = pageout_region((__u32)st, sizeof(struct stat));
  if (page_res != 1) {
    errx(1, "Paging out failed");
  }
  int fstat_res = fstat(fd, st);
  printf("fstat res: %d, size: %d\n", fstat_res, (int)st->st_size);
  if (fstat_res != 0) {
    errx(1, "fstat failed");
  }
  exit(0);
  printf("old fd: %d\n", fd);
  int newfd = dup(fd);
  if (newfd < fd) {
    errx(1, "newfd %d is less than oldfd %d", newfd, fd);
  }
  printf("new fd: %d\n", newfd);
  ssize_t readsize = 0;
  char buf[2] = { 'X', '\0' };

  readsize = read(fd, buf, 1);
  if (readsize != 1) {
    errx(1, "error reading byte 1 using oldfd");
  }

  printf("first byte: '%c'\n", buf[0]);
  readsize = read(newfd, buf, 1);
  if (readsize != 1) {
    errx(1, "error reading byte 2 using oldfd");
  }
  printf("second byte: '%c'\n", buf[0]);

  int res = 0;

  res = close(fd);
  if (res != 0) {
    errx(1, "closing oldfd failed");
  }
  readsize = read(fd, buf, 1);
  if (readsize != -1) {
    errx(1, "read from closed fd worked: '%c'", buf[0]);
  }

  readsize = read(newfd, buf, 1);
  if (readsize != 1) {
    errx(1, "error reading byte 3 using newfd after closing old");
  }
  printf("third byte: '%c'\n", buf[0]);
  res = close(newfd);
  if (res != 0) {
    errx(1, "closing newfd failed");
  }
  newfd = open(argv[2], O_RDONLY, 0644); // reopen with new fd
  if (newfd < 3) {
    errx(1, "error opening file in RDONLY: %d", newfd);
  }

  int newopenfd = open(argv[2], O_WRONLY, 0644);
  if (newopenfd < 3) {
    errx(1, "should be able to open same file twice, creating new fd, got: %d", newopenfd);
  }
  readsize = read(newopenfd, buf, 1);
  if (readsize != -1) {
    errx(1, "shouldn't be able to read with a WRONLY fd");
  }
  res = write(newopenfd, "why hello\n", 10);
  if (res != 10) {
    errx(1, "error writing using newopenfd (WRONLY), result: %d", res);
  }
  res = lseek(newfd, 0, SEEK_SET);
  if (res != 0) {
    errx(1, "error seeking with fd: %d, res: %d", newfd, res);
  }
  char longbuf[11];
  longbuf[10] = 0;
  readsize = read(newfd, longbuf, 10);
  if (readsize != 10) {
    errx(1, "error reading into longbuf: %d", readsize);
  }
  if (strcmp(longbuf, "why hello\n") != 0) {
    errx(1, "strcmp failed");
  }

  char longerbuf[101]; longerbuf[100] = '\0';
  readsize = read(newfd, longerbuf, 100);
  if (readsize == -1) {
    errx(1, "should have read after write");
  }
  if (readsize == 10 || readsize == 100) {
    errx(1, "shouldn't have read specified number of bytes, and shouldn't have reset offset");
  }
  longerbuf[readsize] = '\0';
  printf("read %d chars after write: \"%s\"\n", readsize, longerbuf);

  int dup2_fd = dup2(newfd, 99);
  if (dup2_fd != 99) {
    errx(1, "dup2 should have given proper FD back, got: %d", dup2_fd);
  }
  res = close(newfd);
  if (res != 0) {
    errx(1, "close failed");
  }

  res = read(dup2_fd, buf, 1);
  if (res != 0) {
    errx(1, "reading should have failed due to being at EOF, got: %d", res);
  }
  res = lseek(dup2_fd, 0, SEEK_SET);
  memset(longerbuf, '\0', 101);
  readsize = read(dup2_fd, longerbuf, 100);
  if (readsize <= 0) {
    errx(1, "error reading file from dup2'd fd, got: %d", readsize);
  }
  printf("full buf: %s\n", longerbuf);
}

int main(int argc, char *argv[]) {
  if (strcmp(argv[1], "fcntl") == 0) {
    fcntl_test(argc, argv);
    exit(0);
  } else if (strcmp(argv[1], "pipe") == 0) {
    pipe_test(argc, argv);
    exit(0);
  } else if (strcmp(argv[1], "files") == 0) {
    files_test(argc, argv);
    exit(0);
  } else {
    errx(1, "Usage error! luketest fcntl|pipe|files OPTIONS\n");
  }
}
