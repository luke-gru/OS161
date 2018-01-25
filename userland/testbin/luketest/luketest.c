#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <err.h>

int main(int argc, char *argv[]) {
  if (argc != 2) {
    errx(1, "must give file to write to");
  }
  int fd = open(argv[1], O_RDONLY, 0644);
  if (fd < 3) {
    errx(1, "Error opening file: %s", argv[1]);
  }
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
  newfd = open(argv[1], O_RDONLY, 0644); // reopen with new fd
  if (newfd < 3) {
    errx(1, "error opening file in RDONLY: %d", newfd);
  }

  int newopenfd = open(argv[1], O_WRONLY, 0644);
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

  res = close(newfd);
  if (res != 0) {
    errx(1, "close failed");
  }

  exit(0);
}
