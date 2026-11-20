/* What git asks of POSIX that bionic leaves out, included first in every
 * file (packages/git/recipe.sh: -include).
 *
 * pthread_setcancelstate: run-command.c turns cancellation off around
 * fork. Bionic has no thread cancellation at all (no pthread_cancel), so
 * there is nothing to turn off: the state is always "disabled". */
#pragma once
#include <pthread.h>

#ifndef PTHREAD_CANCEL_ENABLE
#define PTHREAD_CANCEL_ENABLE 0
#define PTHREAD_CANCEL_DISABLE 1
static inline int pthread_setcancelstate(int state, int *old)
{
	(void)state;
	if (old)
		*old = PTHREAD_CANCEL_DISABLE;
	return 0;
}
#endif
