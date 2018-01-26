#ifndef _H_ARGVDATA
#define _H_ARGVDATA

#include <types.h>

#define NARGS_MAX 100
#define ARG_SINGLE_MAX 100

struct argvdata {
	char *buffer; // buffer for array of argument strings
	char *bufend; // 1 past end of buffer
	size_t *offsets; // pointer offsets into argv string buffer
	int nargs; // same as argc, must be at least 1 (progname)
};

struct argvdata *argvdata_create(void);
void argvdata_destroy(struct argvdata *argdata);
void argvdata_debug(struct argvdata *argdata, const char *msg, char *progname);
int argvdata_fill(struct argvdata *argdata, char *progname, char **args, int argc);
int argvdata_fill_from_uspace(struct argvdata *argdata, char *progname, userptr_t argv);

#endif
