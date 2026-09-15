#include "buf.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void oom(void)
{
	fputs("qq: out of memory\n", stderr);
	exit(1);
}

void buf_reserve(struct buf *b, size_t extra)
{
	size_t need, cap;
	char *p;

	if (b->cap - b->len > extra)
		return;
	need = b->len + extra + 1;
	if (need <= b->len)
		oom();
	cap = b->cap ? b->cap : 256;
	while (cap < need)
		cap = cap > SIZE_MAX / 2 ? need : cap * 2;
	if (!(p = realloc(b->data, cap)))
		oom();
	if (!b->data)
		p[0] = '\0'; /* keep the always-terminated promise even before any append */
	b->data = p;
	b->cap = cap;
}

void buf_append(struct buf *b, const char *s, size_t n)
{
	buf_reserve(b, n);
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
}

void buf_puts(struct buf *b, const char *s)
{
	buf_append(b, s, strlen(s));
}

void buf_appendf(struct buf *b, const char *fmt, ...)
{
	va_list ap, ap2;
	int n;

	va_start(ap, fmt);
	va_copy(ap2, ap);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0)
		oom();
	buf_reserve(b, (size_t)n);
	vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
	va_end(ap2);
	b->len += (size_t)n;
}

int buf_read_fd(struct buf *b, int fd, size_t max)
{
	ssize_t n;

	for (;;) {
		buf_reserve(b, 65536);
		n = read(fd, b->data + b->len, b->cap - b->len - 1);
		if (n == 0)
			break;
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		b->len += (size_t)n;
		b->data[b->len] = '\0';
		if (b->len > max)
			return -2;
	}
	return 0;
}

char *buf_steal(struct buf *b)
{
	char *s = b->data;

	if (!s && !(s = calloc(1, 1)))
		oom();
	*b = (struct buf){0};
	return s;
}

void buf_free(struct buf *b)
{
	free(b->data);
	*b = (struct buf){0};
}

char *str_trim(char *s)
{
	size_t n;

	while (isspace((unsigned char)*s))
		s++;
	n = strlen(s);
	while (n && isspace((unsigned char)s[n - 1]))
		s[--n] = '\0';
	return s;
}
