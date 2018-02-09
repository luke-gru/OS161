#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <sys/stat.h>
#include <errno.h>

static int socket_test(int argc, char **argv) {
  (void)argc; (void)argv;
  int fd = socket(AF_INET, SOCK_STREAM, PF_INET);
  if (fd < 3) {
    errx(1, "couldn't create socket: %d - %d (%s)", fd, errno, strerror(errno));
  }

  struct sockaddr_in sockaddr;
  sockaddr.sa_len = sizeof(struct sockaddr_in);
  sockaddr.sa_family = AF_INET;
  strcpy(sockaddr.sa_data, "127.0.0.1:80");

  int bind_res = bind(fd, (const struct sockaddr*)&sockaddr, sizeof(struct sockaddr_in));
  if (bind_res != 0) {
    errx(1, "socket bind() failed: %d (%s)", errno, strerror(errno));
  }
  int listen_res = listen(fd, 100);
  if (listen_res != 0) {
    errx(1, "socket listen() failed: %d (%s)", errno, strerror(errno));
  }

  struct sockaddr_in connected_addr;
  connected_addr.sa_len = sizeof(struct sockaddr_in);
  connected_addr.sa_family = AF_INET;
  memset(connected_addr.sa_data, 0, sizeof(connected_addr.sa_data));
  socklen_t connected_addrlen = sizeof(struct sockaddr_in);
  socklen_t oldlen = connected_addrlen;
  int connected_fd = accept(fd, (struct sockaddr*)&connected_addr, &connected_addrlen);
  if (connected_fd < 0) {
    errx(1, "socket accept() failed: %d (%s)", errno, strerror(errno));
  }
  if (connected_addrlen != oldlen) {
    errx(1, "socket accept() overwrote peer addrlen unnecessarily!");
  }

  printf("Connected socket (fd: %d) has address '%s'\n", connected_fd, connected_addr.sa_data);

  size_t readlen = 10;
  char *buf = malloc(readlen);
  int read_res = read(connected_fd, buf, readlen);
  if (read_res == -1) {
    errx(1, "Error reading from socket: %d (%s)", errno, strerror(errno));
  }
  buf[readlen-1]='\0';
  printf("Read %d bytes from socket: '%s'\n", read_res, buf);

  int write_res = write(connected_fd, buf, readlen);
  if (write_res == -1) {
    errx(1, "Error writing to socket: %d (%s)", errno, strerror(errno));
  }

  int close_res = close(connected_fd);
  if (close_res != 0) {
    errx(1, "socket close() failed for connected fd: %d (%s)", errno, strerror(errno));
  }

  close_res = close(fd);
  if (close_res != 0) {
    errx(1, "socket close() failed for listening socket: %d (%s)", errno, strerror(errno));
  }
  printf("Done, success!\n");
  return 0;
}

static int select_test(int argc, char **argv) {
  (void)argc; (void)argv;
  int pipefd[2];
  pid_t cpid;
  char buf;

  if (pipe(pipefd, 100) == -1) {
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

  if (cpid == 0) {    /* Child reads from pipe, will be blocked because nothing avail for first 5 secs */
    close(pipefd[1]);          /* Close unused write end */

    struct timespec ts;
    __time(&ts.tv_sec, (unsigned long*)&ts.tv_nsec);
    ts.tv_sec += 10; // 10-second timeout
    struct timeval tv;
    TIMESPEC_TO_TIMEVAL(&tv, &ts);
    if (tv.tv_sec < ts.tv_sec) {
      errx(1, "TIMESPEC_TO_TIMEVAL failed");
    }
    struct fd_set readfds;
    FD_ZERO(&readfds);
    int read_fd = pipefd[0];
    FD_SET(read_fd, &readfds);
    //printf("TV values: %ld : %ld\n", (long)tv.tv_sec, (long)tv.tv_usec);
    int select_res = select(read_fd+1, &readfds, NULL, NULL, &tv);
    if (select_res != 1) {
      errx(1, "Read should be ready after 5 seconds: got %d", select_res);
    }
    if (!FD_ISSET(read_fd, &readfds)) {
      errx(1, "&readfds fd_set not modified properly in select() call");
    }
    printf("Child is ready to read, reading and then writing out:\n");
    while (read(pipefd[0], &buf, 1) > 0)
      write(1, &buf, 1);

    write(1, "\n", 1);
    close(pipefd[0]);
    _exit(0);
  } else {                     /* Parent writes argv[1] to pipe */
    close(pipefd[0]);          /* Close unused read end */
    printf("Parent sleeping for 5 seconds\n");
    sleep(5);
    printf("Parent writing after sleep\n");
    write(pipefd[1], argv[1], strlen(argv[1]));
    close(pipefd[1]);          /* Reader will see EOF */
    int exitstatus;
    waitpid(cpid, &exitstatus, 0);              /* Wait for child */
  }
  return 0;
}

// MAP_SHARED with file, msync
static int msync_test(int argc, char **argv) {
  if (argc != 3) {
    errx(1, "Usage error, need to give file to read");
  }
  char *fname = argv[2];
  int fd = open(fname, O_RDWR, 0644);
  if (fd < 3) {
    errx(1, "Open failed: %d", fd);
  }
  struct stat st;
  int stat_res = fstat(fd, &st);
  if (stat_res != 0) {
    errx(1, "fstat failed: %d", stat_res);
  }
  size_t filesize = st.st_size;
  if (filesize < 4) {
    errx(1, "Need a file with at least 4 characters");
  }
  void *startaddr = mmap(filesize+1, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  if (startaddr == MAP_FAILED) {
    errx(1, "mmap call failed: %d (%s)", errno, strerror(errno));
  }
  char *file_contents = malloc(filesize+1);
  int read_res = read(fd, file_contents, filesize);
  if (read_res != (int)filesize) {
    errx(1, "Read failed: %d", read_res);
  }
  char old_char = file_contents[4];
  file_contents[4] = '\0';
  if (strcmp(file_contents, "NEW!") == 0) {
    errx(1, "File contents are already 'NEW!'");
  }
  char *startmem = (char*)startaddr;
  startmem[0] = 'N'; startmem[1] = 'E'; startmem[2] = 'W'; startmem[3] = '!';
  int msync_res = msync(startaddr, 4, 0);
  if (msync_res != 0) {
    errx(1, "msync call failed: %d", msync_res);
  }
  char *new_file_contents = malloc(filesize+1);
  lseek(fd, 0, SEEK_SET);
  read(fd, new_file_contents, filesize);
  new_file_contents[4] = '\0';
  if (strcmp(new_file_contents, "NEW!") != 0) {
    errx(1, "New file contents aren't correct");
  }
  file_contents[4] = old_char;
  lseek(fd, 0, SEEK_SET);
  int write_old_res = write(fd, file_contents, filesize);
  if (write_old_res != (int)filesize) {
    printf("Writing old file contents back to file failed with %d\n", write_old_res);
  }
  printf("msync properly synced file\n");
  return 0;
}

// MAP_PRIVATE with file
static int mmap_test6(int argc, char **argv) {
  if (argc != 3) {
    errx(1, "Usage error, need to give file to read");
  }
  char *fname = argv[2];
  int fd = open(fname, O_RDWR, 0644);
  if (fd < 3) {
    errx(1, "Open failed: %d", fd);
  }
  struct stat st;
  int stat_res = fstat(fd, &st);
  if (stat_res != 0) {
    errx(1, "fstat failed: %d", stat_res);
  }
  size_t filesize = st.st_size;
  void *startaddr = mmap(filesize+1, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (startaddr == MAP_FAILED) {
    errx(1, "mmap call failed: %d (%s)", errno, strerror(errno));
  }
  char *file_contents = malloc(filesize+1);
  int read_res = read(fd, file_contents, filesize);
  file_contents[filesize] = '\0';
  if (read_res != (int)filesize) {
    errx(1, "Read failed: %d", read_res);
  }
  for (size_t i = 0; i < filesize; i++) {
    if (file_contents[i] != ((char*)startaddr)[i]) {
      errx(1, "Invalid memory in iteration %d", (int)i);
    }
  }
  if (strcmp(file_contents, startaddr) != 0) {
    errx(1, "strcmp failed");
  }
  return 0;
}

// mmap shareable with child with MAP_SHARED
static int mmap_test5(int argc, char **argv) {
  (void)argc;
  (void)argv;
  void *startaddr = mmap(2000, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, 0, 0);
  if (startaddr == MAP_FAILED) {
    errx(1, "mmap call failed: %d (%s)", errno, strerror(errno));
  }
  memset(startaddr, 'A', 10);
  ((char*)startaddr)[10] = '\0';
  printf("10 A's:\n");
  printf("  %s\n", (char*)startaddr);

  pid_t pid = fork();
  if (pid == 0) { // child, shouldn't have access
    printf("map private should fault on %s\n", (char*)startaddr);
    errx(1, "Previous line should have faulted!");
  } else { // parent
    int exitstatus;
    waitpid(pid, &exitstatus, 0);
    if (exitstatus == 0) {
      errx(1, "Child should have exited with non-zero exitstatus");
    }
    printf("  in parent, still A: %s\n", (char*)startaddr);
  }
  return 0;
}

// accessing munmapped memory results in a fault
static int mmap_test4(int argc, char **argv) {
  (void)argc;
  (void)argv;
  void *startaddr = mmap(2000, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, 0, 0);
  if (startaddr == MAP_FAILED) {
    errx(1, "mmap (MAP_PRIVATE) call failed: %d (%s)", errno, strerror(errno));
  }
  memset(startaddr, 'C', 10);
  ((char*)startaddr)[10] = '\0';
  printf("map private should work, have 10 C's: %s\n", (char*)startaddr);
  int res = munmap(startaddr);
  if (res != 0) {
    errx(1, "munmap call failed: %d (%s)", errno, strerror(errno));
  }
  printf("map private should fault on %s\n", (char*)startaddr);
  errx(1, "Shouldn't get here, should have faulted on previous line!");

  return 0;
}

// munmap works (return value)
static int mmap_test3(int argc, char **argv) {
  (void)argc;
  (void)argv;
  void *startaddr = mmap(2000, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_SHARED, 0, 0);
  if (startaddr == MAP_FAILED) {
    errx(1, "mmap call failed: %d (%s)", errno, strerror(errno));
  }
  int res = munmap(startaddr);
  if (res != 0) {
    errx(1, "munmap call failed: %d (%s)", errno, strerror(errno));
  }
  return 0;
}

// mmap physically unmapped by creator's exit, even if shared with alive child
static int mmap_test2(int argc, char **argv) {
  (void)argc;
  (void)argv;
  void *startaddr = mmap(2000, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_SHARED, 0, 0);
  if (startaddr == MAP_FAILED) {
    errx(1, "mmap call failed: %d (%s)", errno, strerror(errno));
  }
  memset(startaddr, 'A', 20);
  ((char*)startaddr)[20] = '\0';
  printf("20 A's:\n");
  printf("  %s\n", (char*)startaddr);

  pid_t pid = fork();
  if (pid == 0) { // child
    sleep(1);
    memset(startaddr, 'B', 20); // should fail, region should be unmapped by parent's exit
    printf("in child, mapping should be destroyed: %s\n", (char*)startaddr);
  } else { // parent
    exit(0);
  }
  return 0;
}

// mmap shareable with child with MAP_SHARED
static int mmap_test1(int argc, char **argv) {
  (void)argc;
  (void)argv;
  void *startaddr = mmap(2000, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_SHARED, 0, 0);
  if (startaddr == MAP_FAILED) {
    errx(1, "mmap call failed: %d (%s)", errno, strerror(errno));
  }
  memset(startaddr, 'A', 20);
  ((char*)startaddr)[20] = '\0';
  printf("20 A's:\n");
  printf("  %s\n", (char*)startaddr);

  pid_t pid = fork();
  if (pid == 0) { // child
    memset(startaddr, 'B', 20);
    ((char*)startaddr)[20] = '\0';
    printf("  in child, now B: %s\n", (char*)startaddr);

  } else { // parent
    int exitstatus;
    waitpid(pid, &exitstatus, 0);
    printf("  in parent, now B: %s\n", (char*)startaddr);
  }
  return 0;
}

static int sleep_test(int argc, char **argv) {
  (void)argc;
  (void)argv;
  while (1) {
    sleep(5);
    printf("Slept for 5 seconds, now up\n");
  }
  return 0;
}

static int clone_entry(void *data1) {
  (void)data1;
  printf("Clone entry!\n");
  sleep(5);
  char *testaddrspace = malloc(10);
  snprintf(testaddrspace, 10, "%d", 100);
  printf("Clone test address space heap not destroyed: %s\n", testaddrspace);
  return 0;
}

static void clone_test(int argc, char **argv) {
  (void)argc;
  (void)argv;
  size_t stack_size = 4096;
  __u32 child_stacktop = (__u32)malloc(stack_size);
  child_stacktop += stack_size;
  int clone_res = clone(clone_entry, (void*)child_stacktop, stack_size, 0);
  sleep(2);
  printf("Clone result: %d\n", clone_res);
  if (clone_res <= 0) {
    errx(1, "Error running clone()\n");
  }
}

int atexit_num = 2;
static void atexit_printer1(void) {
  if (atexit_num != 1) {
    printf("Uh oh, atexit_num isn't 1!\n");
  }
  printf("Bye from %d!\n", atexit_num);
}
static void atexit_printer2(void) {
  printf("Bye from %d!\n", atexit_num);
  atexit_num--;
}

static void atexit_test(int argc, char **argv) {
  (void)argc; (void)argv;
  int res = atexit(atexit_printer1);
  if (res != 0) {
    errx(1, "Unable to register atexit handler 1\n");
  }
  res = atexit(atexit_printer2); // should run first
  if (res != 0) {
    errx(1, "Unable to register atexit handler 2\n");
  }
  exit(0);
}

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
  } else if (strcmp(argv[1], "atexit") == 0) {
    atexit_test(argc, argv);
    exit(0);
  } else if (strcmp(argv[1], "clone") == 0) {
    clone_test(argc, argv);
    exit(0);
  } else if (strcmp(argv[1], "sleep") == 0) {
    sleep_test(argc, argv);
  } else if (strcmp(argv[1], "mmap1") == 0) {
    mmap_test1(argc, argv);
  } else if (strcmp(argv[1], "mmap2") == 0) {
    mmap_test2(argc, argv);
  } else if (strcmp(argv[1], "mmap3") == 0) {
    mmap_test3(argc, argv);
  } else if (strcmp(argv[1], "mmap4") == 0) {
    mmap_test4(argc, argv);
  } else if (strcmp(argv[1], "mmap5") == 0) {
    mmap_test5(argc, argv);
  } else if (strcmp(argv[1], "mmap6") == 0) {
    mmap_test6(argc, argv);
  } else if (strcmp(argv[1], "msync") == 0) {
    msync_test(argc, argv);
  } else if (strcmp(argv[1], "select") == 0) {
    select_test(argc, argv);
  } else if (strcmp(argv[1], "socket") == 0) {
    socket_test(argc, argv);
  } else {
    errx(1, "Usage error! luketest fcntl|pipe|files|atexit|sleep|mmap[1-6]|msync|select|socket OPTIONS\n");
  }
}
