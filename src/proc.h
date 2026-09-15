#ifndef QQ_PROC_H
#define QQ_PROC_H

#include "buf.h"

#include <stddef.h>

/* Milliseconds on a monotonic clock. */
long long proc_now_ms(void);

enum { PROC_OK = 0, PROC_ERROR = -1, PROC_TIMEOUT = -2 };

struct proc {
	struct buf out; /* child's stdout */
	struct buf err; /* child's stderr */
	int status;     /* waitpid status once it exited */
	int truncated;  /* output beyond the cap was discarded */
};

/* Run argv (found through PATH), writing input to its stdin and collecting
 * stdout and stderr, keeping at most max_output bytes of each. Returns PROC_OK
 * once the child exits, PROC_TIMEOUT if it was still running at deadline_ms
 * (proc_now_ms time) and had to be killed, or PROC_ERROR with a message in err.
 * Always initializes p; release it with proc_free(). */
int proc_run(const char *const argv[], const char *input, size_t input_len,
	     size_t max_output, long long deadline_ms, struct proc *p, char *err, size_t errlen);

void proc_free(struct proc *p);

#endif
