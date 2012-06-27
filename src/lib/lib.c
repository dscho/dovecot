/* Copyright (c) 2001-2012 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "env-util.h"
#include "hostpid.h"
#include "ipwd.h"
#include "process-title.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>

size_t nearest_power(size_t num)
{
	size_t n = 1;

	i_assert(num <= ((size_t)1 << (CHAR_BIT*sizeof(size_t) - 1)));

	while (n < num) n <<= 1;
	return n;
}

void lib_init(void)
{
	struct timeval tv;

	/* standard way to get rand() return different values. */
	if (gettimeofday(&tv, NULL) < 0)
		i_fatal("gettimeofday(): %m");
	srand((unsigned int) (tv.tv_sec ^ tv.tv_usec ^ getpid()));

	data_stack_init();
	hostpid_init();
}

int close_keep_errno(int *fd)
{
	int ret, old_errno = errno;

	i_assert(*fd != -1);

	ret = close(*fd);
	*fd = -1;
	errno = old_errno;
	return ret;
}

void lib_deinit(void)
{
	ipwd_deinit();
	data_stack_deinit();
	env_deinit();
	failures_deinit();
	process_title_deinit();
}
