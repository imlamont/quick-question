#ifndef QQ_LOG_H
#define QQ_LOG_H

#include <stddef.h>

struct cJSON;

/* Debug log for -L. Every entry is one line: a local timestamp to the
 * millisecond, an event name, then its details, so the file greps cleanly.
 * Until log_open() succeeds every call here does nothing, which is what makes
 * the logging calls scattered through qq free when -L is absent.
 *
 * The log holds the whole exchange, including the prompts, any file contents
 * read and any command output, so it is as private as the conversation. The API
 * key is never in it: that travels in a header, which qq does not log. */

/* Open path for appending. 0, or -1 with a message in err. */
int log_open(const char *path, char *err, size_t errlen);

/* 1 once a log is open, for skipping work only the log would need. */
int log_on(void);

void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* The command line as invoked, one argument after another, each quoted the way
 * a shell would need it. Logged first, so a log read back later begins with
 * what was actually asked for. */
void log_command(int argc, char **argv);

/* event followed by json, compact, on one line. */
void log_json(const char *event, const struct cJSON *json);

/* text with backslashes, newlines and other control characters escaped, so an
 * entry can never span lines or move the terminal it is later read on.
 * malloc'd, and empty for NULL. */
char *log_escape(const char *text, size_t len);

void log_close(void);

#endif
