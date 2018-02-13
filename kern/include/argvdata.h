#ifndef _H_ARGVDATA
#define _H_ARGVDATA

#include <types.h>

#define NARGS_MAX 100
#define ARG_SINGLE_MAX 100

// NOTE: this struct isn't just used for argv, it's also used for envp (the userspace environment array.
// See getenv(2) for more details)
struct argvdata {
	char *buffer; // buffer for array of argument strings
	char *bufend; // 1 past end of buffer
	size_t *offsets; // pointer offsets into argv string buffer
	int nargs; // same as argc, must be at least 1 (progname)
	int nargs_max; // max number of arguments we can ever put on userspace stack.
};

struct argvdata *argvdata_create(void);
void argvdata_destroy(struct argvdata *argdata);
void argvdata_debug(struct argvdata *argdata, const char *msg, char *progname);
int argvdata_fill(struct argvdata *argdata, char *progname, char **args, int argc);
int argvdata_fill_from_uspace(struct argvdata *argdata, char *progname, userptr_t argv);

#endif
