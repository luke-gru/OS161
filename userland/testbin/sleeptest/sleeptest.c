#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  printf("sleeping\n");
  sleep(3);
  printf("done\n");
  exit(0);
}
