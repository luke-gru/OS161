/*
 * Copyright (c) 2013
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

/*
 * getenv(): ANSI C
 *
 * Get an environment variable.
 */

/*
 * This is initialized by crt0, though it actually lives in errno.c
 */
extern char **__environ;

/*
 * This is what we use by default if the kernel didn't supply an
 * environment.
 */
static const char *__default_environ[] = {
	"PATH=/bin:/sbin:/testbin",
	"SHELL=/bin/sh",
	"TERM=vt220",
	NULL
};

static size_t env_max_vars = 100;

static int getenv_idx(const char *var) {
	size_t varlen, thislen;
	char *s;

	if (__environ == NULL) {
		__environ = (char **)__default_environ;
	}
	varlen = strlen(var);
	for (size_t i=0; i<env_max_vars; i++) {
		if (__environ[i] == NULL) { continue; }
		s = strchr(__environ[i], '=');
		if (s == NULL) {
			/* ? */
			continue;
		}
		thislen = s - __environ[i];
		if (thislen == varlen && memcmp(__environ[i], var, thislen) == 0) {
			return (int)i;
		}
	}
	return -1;
}

// returns ENV value for given ENV name
char *getenv(const char *var) {
	int env_idx = getenv_idx(var);
	if (env_idx == -1) return NULL;
	char *s = strchr(__environ[env_idx], '=');
	if (s == NULL) {
		return NULL;
	}
	return s+1;
}

static int env_first_avail_idx(void) {
	if (__environ == NULL) {
		__environ = (char **)__default_environ;
	}
	for (size_t i = 0;	i < env_max_vars; i++) {
		if (__environ[i] == NULL) {
			return (int)i;
		}
	}
	return -1;
}

int setenv(const char *name, const char *value, int overwrite) {
	char *existing = NULL;
	if ((existing = getenv(name)) != NULL && !overwrite) {
		errno = 0;
		return 0;
	}
	char *s = NULL;
	char *var_p = NULL;
	if (!existing) {
		s = strchr(name, '=');
		if (s != NULL) {
			errno = EINVAL;
			return -1;
		}
		int first_avail_idx = env_first_avail_idx();
		if (first_avail_idx == -1) {
			errno = ENOMEM;
			return -1;
		}
		size_t buflen = strlen(name)+strlen(value)+2;
		var_p = malloc(buflen);
		if (!var_p) {
			errno = ENOMEM;
			return -1;
		}
		snprintf(var_p, buflen, "%s=%s", name, value);
		__environ[first_avail_idx] = var_p;
		errno = 0;
		return 0;
	} else {
		for (int i = 0; (var_p = __environ[i]) != NULL; i++) {
			s = strchr(var_p, '=');
			if (s == NULL) { continue; /* ? */ }
			size_t namelen = s - var_p;
			if (memcmp(var_p, name, namelen) != 0) {
				continue;
			}
			size_t buflen = strlen(name)+strlen(value)+2; // 1 for trailing NULL, 1 for '=' separator
			if ((strlen(var_p)+1) < buflen) {
				var_p = malloc(buflen);
				if (!var_p) {
					errno = ENOMEM;
					return -1;
				}
				__environ[i] = var_p;
			}
			snprintf(var_p, buflen, "%s=%s", name, value);
			errno = 0;
			return 0;
		}
	}
	errno = EINVAL;
	return -1;
}

int unsetenv(const char *name) {
	int idx = getenv_idx(name);
	if (idx == -1) {
		errno = 0;
		return 0;
	}
	// NOTE: we don't free the memory right now, we probably should but we'd have to keep
	// track of which strings were allocated with malloc(), because the initial strings weren't...
	// they're placed on the user stack by the kernel.
	__environ[idx] = NULL;
	errno = 0;
	return 0;
}

int putenv(char *entry) {
	char *s = s = strchr(entry, '=');
	if (!s) {
		errno = EINVAL;
		return -1;
	}
	*s = '\0'; // split the string in half so we can search for the ENV name
	char *name = entry;
	int idx = getenv_idx(name);
	*s = '='; // add the separator back
	if (idx == -1) { // new entry
		int avail_idx = env_first_avail_idx();
		if (avail_idx == -1) {
			errno = ENOMEM;
			return -1;
		}
		__environ[avail_idx] = entry;
	} else { // overwriting entry
		__environ[idx] = entry;
	}
	errno = 0;
	return 0;
}
