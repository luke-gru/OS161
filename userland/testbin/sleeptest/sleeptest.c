#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  int res;

  char *readbuf = malloc(50);
  memset(readbuf, 'A', 50);
  readbuf[49] = '\0';
  printf("Calling getcwd()\n");
  getcwd(readbuf, 50);
  printf("Result from getcwd: %s\n", readbuf);
  exit(0);

  char *buf = malloc(2000);
  memset(buf, 'A', 2000);

  printf("paging out region\n");
  res = pageout_region((__u32)buf, 2000);
  printf("page result: %d\n", res);

  printf("accessing region\n");
  char letter = buf[1];
  printf("got: %c\n", letter);

  printf("locking (pinning) region\n");
  res = lock_region((__u32)buf, 2000);
  printf("got: %d\n", res);

  printf("paging out locked region, should fail\n");
  res = pageout_region((__u32)buf, 2000);
  printf("page result: %d\n", res);

  // printf("mallocing 5 pages\n");
  // for (int j = 0; j < 5; j++) {
  //   char *chars = malloc(2000);
  //   memset(chars, 'A', 2000);
  //   printf("sleeping\n");
  //   sleep(3);
  //   printf("checking\n");
  //   for (int i = 0; i < 2000; i++) {
  //     if (chars[i] != 'A') {
  //       printf("Invalid memory at index %d!\n", i);
  //       exit(1);
  //     }
  //   }
  //   //free(chars);
  // }
  printf("done\n");
  exit(0);
}
