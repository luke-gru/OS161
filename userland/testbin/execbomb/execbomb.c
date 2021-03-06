#include <unistd.h>
#include <err.h>
#include <time.h>
#include <test161/test161.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
  if (argc == 1) {
    const char *args[3] = { "testbin/execbomb", "1", NULL };
    printf("execbomb starting...\n");
    execv("testbin/execbomb", (char *const *)args);
  } else {
    if (argc < 2)
      err(1, "Invalid # of arguments");
    int iter = atoi(argv[1]);
    if (iter < 1 || iter > 100) {
      errx(1, "Invalid iteration: %d\n", iter);
    }
    iter++;
    if (iter == 100) {
      printf("SUCCESS\n");
      exit(0);
    } else {
      const char *args[3] = { "testbin/execbomb", "  ", NULL };
      snprintf((char*)args[1], 4, "%d", iter);
      printf("execbomb iter %d\n", iter);
      execv("testbin/execbomb", (char *const *)args);
    }

  }
}
