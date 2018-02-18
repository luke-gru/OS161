#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>

const char *in_sig_msg = "Got into my handler for sig %d!\n";
int handled_sigusr1 = 0;

static void my_sigusr1_handler_siginfo(int signo, siginfo_t *siginfo, void *restorer) {
  (void)restorer;
  (void)siginfo;
  printf(in_sig_msg, signo);
  handled_sigusr1 = 1;
}

static void my_sigusr1_handler(int signo) {
  printf(in_sig_msg, signo);
  handled_sigusr1 = 1;
}

size_t altstack_btm;
size_t altstack_top;
static void my_sigusr1_handler_altstack(int signo) {
  printf(in_sig_msg, signo);
  struct sigaction sigact;
  struct sigaction *sigact_p = &sigact;
  if ((size_t)sigact_p < altstack_btm || (size_t)sigact_p > altstack_top) {
    errx(1, "not running on alternate stack!");
  }
  handled_sigusr1 = 1;
}

// run this in the background from the shell:
// $ b testbin/luketest sigaltstack
// pid: 3
// $ sig SIGUSR1 3
// Successfully sent signal
static int sigaltstack_test(int argc, char **argv) {
  (void)argc;
  (void)argv;
  struct sigaction sigact;
  memset(&sigact, 0, sizeof(sigact));
  sigact.sa_flags = SA_ONSTACK;
  sigact.sa_handler = my_sigusr1_handler_altstack;
  int sigact_res = sigaction(SIGUSR1, &sigact, NULL);
  if (sigact_res != 0) {
    errx(1, "sigaction returned non-0 when setting new sigaction: %d, errno=%d (%s)\n", sigact_res, errno, strerror(errno));
  }
  stack_t altstack;
  memset(&altstack, 0, sizeof(altstack));
  void *stackptr = malloc(MINSIGSTKSZ);
  if (!stackptr) {
    errx(1, "malloc error");
  }
  altstack_btm = (size_t)stackptr;
  altstack_top = altstack_btm + MINSIGSTKSZ;
  altstack.ss_sp = stackptr;
  altstack.ss_size = MINSIGSTKSZ;
  altstack.ss_flags = 0;
  int setup_stack_res = sigaltstack((const stack_t*)&altstack, NULL);
  if (setup_stack_res != 0) {
    errx(1, "sigaltstack returned non-0: %d: errno=%d (%s)", setup_stack_res, errno, strerror(errno));
  }
  while (1) {
    sleep(5);
    if (handled_sigusr1) {
      printf("handled sigusr1\n");
      exit(0);
    }
  }
}

// run this in the background from the shell:
// $ b testbin/luketest sigaction
// pid: 3
// $ sig SIGUSR1 3
// Successfully sent signal
static int sigaction_test(int argc, char **argv) {
  (void)argc;
  (void)argv;
  struct sigaction sigact;
  memset(&sigact, 0, sizeof(sigact));
  sigact.sa_flags = SA_SIGINFO|SA_RESETHAND;
  sigact.sa_sigaction = my_sigusr1_handler_siginfo;
  int sigact_res = sigaction(SIGUSR1, &sigact, NULL);
  if (sigact_res != 0) {
    errx(1, "sigaction returned non-0 when setting new sigaction: %d, errno=%d (%s)\n", sigact_res, errno, strerror(errno));
  }
  while (1) {
    sleep(5);
    if (handled_sigusr1) {
      printf("handled sigusr1\n");
      bzero(&sigact, sizeof(sigact));
      sigact_res = sigaction(SIGUSR1, NULL, &sigact);
      if (sigact_res != 0) {
        errx(1, "sigaction returned non-0 when getting current sigaction value: %d, errno=%d (%s)\n", sigact_res, errno, strerror(errno));
      }
      if (sigact.sa_handler != SIG_DFL || sigact.sa_sigaction != 0) {
        errx(1, "SA_RESETHAND didn't reset the handler to the default after calling the signal handler");
      }
      exit(0);
    }
  }
}

// run this in the background from the shell:
// $ b testbin/luketest pause
// pid: 3
// $ sig SIGUSR1 3
// Successfully sent signal
static int pause_test(int argc, char **argv) {
  (void)argc;
  (void)argv;
  void *res = signal(SIGUSR1, my_sigusr1_handler);
  if (res == SIG_ERR) {
    errx(1, "Error setting signal handler for SIGUSR1");
  }
  if (res != SIG_DFL) {
    errx(1, "previous handler should have been SIG_DFL");
  }
  printf("Pausing...\n");
  int pause_res = pause();
  printf("Woke up from pause\n");
  if (pause_res != -1 || errno != EINTR) {
    errx(1, "bad return value or errno not properly set: ret=%d, errno=%d (%s)\n", pause_res, errno, strerror(errno));
  }
  if (handled_sigusr1) {
    printf("handled sigusr1, exiting\n");
    exit(0);
  } else {
    errx(1, "handled_sigusr1 var should be set");
  }
}

// run this in the background from the shell:
// $ b testbin/luketest signal
// pid: 3
// $ sig SIGUSR1 3
// Successfully sent signal
static int signal_test(int argc, char **argv) {
  (void)argc;
  (void)argv;
  void *res = signal(SIGUSR1, my_sigusr1_handler);
  if (res == SIG_ERR) {
    errx(1, "Error setting signal handler for SIGUSR1");
  }
  while (1) {
    sleep(5);
    if (handled_sigusr1) {
      printf("handled sigusr1, exiting\n");
      exit(0);
    }
  }
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
  } else if (strcmp(argv[1], "signal") == 0) {
    signal_test(argc, argv);
  } else if (strcmp(argv[1], "pause") == 0) {
    pause_test(argc, argv);
  } else if (strcmp(argv[1], "sigaction") == 0) {
    sigaction_test(argc, argv);
  } else if (strcmp(argv[1], "sigaltstack") == 0) {
    sigaltstack_test(argc, argv);
  } else {
    errx(1, "Usage error! luketest fcntl|pipe|files|atexit|sleep|mmap[1-5]|signal OPTIONS\n");
  }
}
