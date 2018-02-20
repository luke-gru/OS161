#include <unistd.h>
#include <stdio.h>

static int __attribute__ ((noinline)) subroutine_call(int argc, char **argv) {
  (void)argc;
  (void)argv;
  for (int i = 0; i < 100; i++) {
    i += 1;
    printf("i: %d\n", i);
  }
  return 0;
}

int main(int argc, char *argv[]) {
  int res = subroutine_call(argc, argv);
  _exit(res);
}
