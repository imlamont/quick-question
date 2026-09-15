#ifndef QQ_BUF_H
#define QQ_BUF_H

#include <stddef.h>

/* Growable byte buffer. data is always NUL-terminated once non-NULL.
 * Allocation failure is fatal (prints and exits), so callers need no checks. */
struct buf {
	char *data;
	size_t len;
	size_t cap;
};

/* Ensure room for extra bytes plus the terminating NUL. */
void buf_reserve(struct buf *b, size_t extra);
void buf_append(struct buf *b, const char *s, size_t n);
void buf_puts(struct buf *b, const char *s);
void buf_appendf(struct buf *b, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

/* Read fd until EOF. Returns 0, -1 on read error (errno set), or -2 if more
 * than max bytes arrive. */
int buf_read_fd(struct buf *b, int fd, size_t max);

/* Hand ownership of the data to the caller (never NULL) and reset b. */
char *buf_steal(struct buf *b);
void buf_free(struct buf *b);

/* Trim leading and trailing whitespace in place; returns the new start. */
char *str_trim(char *s);

#endif
