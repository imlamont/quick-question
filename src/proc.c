#include "proc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define KILL_GRACE_MS 2000

long long proc_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void close_fd(int *fd)
{
	if (*fd >= 0) {
		close(*fd);
		*fd = -1;
	}
}

static void close_pipe(int p[2])
{
	close_fd(&p[0]);
	close_fd(&p[1]);
}

/* pipe() with both ends close-on-exec: only the dup2'd copies survive exec. */
static int cloexec_pipe(int p[2])
{
	if (pipe(p))
		return -1;
	fcntl(p[0], F_SETFD, FD_CLOEXEC);
	fcntl(p[1], F_SETFD, FD_CLOEXEC);
	return 0;
}

static void set_nonblock(int fd)
{
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

/* Read what is available on a non-blocking fd into b, keeping at most max
 * bytes and discarding the rest; close the fd on EOF or error. */
static void drain(int *fd, struct buf *b, size_t max, int *truncated)
{
	char discard[4096];

	for (;;) {
		ssize_t n;

		if (b->len < max) {
			size_t room = max - b->len;

			buf_reserve(b, room < 65536 ? room : 65536);
			if (room > b->cap - b->len - 1)
				room = b->cap - b->len - 1;
			n = read(*fd, b->data + b->len, room);
			if (n > 0) {
				b->len += (size_t)n;
				b->data[b->len] = '\0';
				continue;
			}
		} else {
			n = read(*fd, discard, sizeof discard);
			if (n > 0) {
				*truncated = 1;
				continue;
			}
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n == 0 || errno != EAGAIN)
			close_fd(fd);
		return;
	}
}

/* Reap pid, giving up at deadline. Returns 0 once reaped, -1 on timeout. */
static int wait_until(pid_t pid, int *status, long long deadline)
{
	for (;;) {
		pid_t r = waitpid(pid, status, WNOHANG);

		if (r == pid || (r < 0 && errno != EINTR))
			return 0;
		if (proc_now_ms() >= deadline)
			return -1;
		nanosleep(&(struct timespec){ .tv_nsec = 1000000L }, NULL);
	}
}

static void terminate(pid_t pid, int *status)
{
	kill(pid, SIGTERM);
	if (wait_until(pid, status, proc_now_ms() + KILL_GRACE_MS)) {
		kill(pid, SIGKILL);
		while (waitpid(pid, status, 0) < 0 && errno == EINTR)
			;
	}
}

int proc_run(const char *const argv[], const char *input, size_t input_len,
	     size_t max_output, long long deadline_ms, struct proc *p, char *err, size_t errlen)
{
	int in[2] = {-1, -1}, out[2] = {-1, -1}, ep[2] = {-1, -1};
	int rc = PROC_ERROR, timed_out = 0, broken = 0;
	size_t off = 0;
	pid_t pid;

	*p = (struct proc){0};
	if (cloexec_pipe(in) || cloexec_pipe(out) || cloexec_pipe(ep)) {
		snprintf(err, errlen, "pipe: %s", strerror(errno));
		goto out;
	}

	if ((pid = fork()) < 0) {
		snprintf(err, errlen, "fork: %s", strerror(errno));
		goto out;
	}
	if (pid == 0) {
		int saved;

		if (dup2(in[0], STDIN_FILENO) < 0 || dup2(out[1], STDOUT_FILENO) < 0 ||
		    dup2(ep[1], STDERR_FILENO) < 0)
			_exit(126);
		execvp(argv[0], (char *const *)argv);
		saved = errno;
		dprintf(STDERR_FILENO, "%s: %s\n", argv[0], strerror(saved));
		_exit(saved == ENOENT ? 127 : 126);
	}

	/* A child that exits early must not kill us through a broken stdin pipe. */
	signal(SIGPIPE, SIG_IGN);
	close_fd(&in[0]);
	close_fd(&out[1]);
	close_fd(&ep[1]);
	set_nonblock(in[1]);
	set_nonblock(out[0]);
	set_nonblock(ep[0]);
	if (input_len == 0)
		close_fd(&in[1]);

	/* Feed stdin and collect stdout/stderr concurrently so neither side
	 * can block on a full pipe. */
	while (out[0] >= 0 || ep[0] >= 0) {
		struct pollfd pfd[3];
		nfds_t n = 0;
		long long left = deadline_ms - proc_now_ms();

		if (left <= 0) {
			timed_out = 1;
			break;
		}
		if (in[1] >= 0)
			pfd[n++] = (struct pollfd){ .fd = in[1], .events = POLLOUT };
		if (out[0] >= 0)
			pfd[n++] = (struct pollfd){ .fd = out[0], .events = POLLIN };
		if (ep[0] >= 0)
			pfd[n++] = (struct pollfd){ .fd = ep[0], .events = POLLIN };

		if (poll(pfd, n, left > INT_MAX ? INT_MAX : (int)left) < 0) {
			if (errno == EINTR)
				continue;
			snprintf(err, errlen, "poll: %s", strerror(errno));
			broken = 1;
			break;
		}
		for (nfds_t i = 0; i < n; i++) {
			if (!pfd[i].revents)
				continue;
			if (pfd[i].fd == in[1]) {
				ssize_t w = write(in[1], input + off, input_len - off);

				if (w > 0)
					off += (size_t)w;
				if (off == input_len || (w < 0 && errno != EAGAIN && errno != EINTR))
					close_fd(&in[1]);
			} else if (pfd[i].fd == out[0]) {
				drain(&out[0], &p->out, max_output, &p->truncated);
			} else {
				drain(&ep[0], &p->err, max_output, &p->truncated);
			}
		}
	}
	close_fd(&in[1]);
	close_fd(&out[0]);
	close_fd(&ep[0]);

	if (!timed_out && !broken && wait_until(pid, &p->status, deadline_ms))
		timed_out = 1;
	if (timed_out || broken) {
		terminate(pid, &p->status);
		rc = timed_out ? PROC_TIMEOUT : PROC_ERROR;
		goto out;
	}
	rc = PROC_OK;
out:
	close_pipe(in);
	close_pipe(out);
	close_pipe(ep);
	return rc;
}

void proc_free(struct proc *p)
{
	buf_free(&p->out);
	buf_free(&p->err);
}
