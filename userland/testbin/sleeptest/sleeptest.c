#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  printf("mallocing 5 pages\n");
  for (int j = 0; j < 5; j++) {
    char *chars = malloc(2000);
    memset(chars, 'A', 2000);
    printf("sleeping\n");
    sleep(3);
    printf("checking\n");
    for (int i = 0; i < 2000; i++) {
      if (chars[i] != 'A') {
        printf("Invalid memory at index %d!\n", i);
        exit(1);
      }
    }
    free(chars);
  }
  printf("done\n");
  exit(0);
}
