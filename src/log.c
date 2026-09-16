#include "log.h"
#include "buf.h"

#include <cjson/cJSON.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static FILE *out;

int log_open(const char *path, char *err, size_t errlen)
{
	if (!(out = fopen(path, "a"))) {
		snprintf(err, errlen, "cannot open log %s: %s", path, strerror(errno));
		return -1;
	}
	return 0;
}

int log_on(void)
{
	return out != NULL;
}

/* "2026-09-16T00:31:07.482-0400 ", or "?" if the clock is unreadable. */
static void stamp(void)
{
	struct timespec ts;
	struct tm tm;
	char when[64];

	if (clock_gettime(CLOCK_REALTIME, &ts) || !localtime_r(&ts.tv_sec, &tm)) {
		fputs("? ", out);
		return;
	}
	strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", &tm);
	fprintf(out, "%s.%03ld", when, ts.tv_nsec / 1000000);
	strftime(when, sizeof when, "%z", &tm);
	fprintf(out, "%s ", when);
}

void log_printf(const char *fmt, ...)
{
	va_list ap;

	if (!out)
		return;
	stamp();
	va_start(ap, fmt);
	vfprintf(out, fmt, ap);
	va_end(ap);
	fputc('\n', out);
	/* Flushed per entry, so a log still explains a run that was interrupted. */
	fflush(out);
}

void log_command(int argc, char **argv)
{
	static const char plain[] = "-_./:=@+,";
	struct buf b = {0};

	if (!out)
		return;
	for (int i = 0; i < argc; i++) {
		/* Escaped first, so the quoting below wraps text that is already
		 * safe to put on one line. */
		char *arg = log_escape(argv[i], strlen(argv[i]));
		int bare = *arg != '\0';

		for (const char *p = arg; bare && *p; p++)
			bare = isalnum((unsigned char)*p) || strchr(plain, *p) != NULL;
		if (i)
			buf_puts(&b, " ");
		if (bare) {
			buf_puts(&b, arg);
		} else {
			buf_puts(&b, "'");
			for (const char *p = arg; *p; p++) {
				if (*p == '\'')
					buf_puts(&b, "'\\''");
				else
					buf_append(&b, p, 1);
			}
			buf_puts(&b, "'");
		}
		free(arg);
	}
	log_printf("command %s", b.data ? b.data : "");
	buf_free(&b);
}

void log_json(const char *event, const cJSON *json)
{
	char *text = json ? cJSON_PrintUnformatted(json) : NULL;

	if (!out)
		return;
	log_printf("%s %s", event, text ? text : "(none)");
	cJSON_free(text);
}

char *log_escape(const char *text, size_t len)
{
	struct buf b = {0};

	for (size_t i = 0; text && i < len; i++) {
		unsigned char c = (unsigned char)text[i];

		switch (c) {
		case '\\': buf_puts(&b, "\\\\"); break;
		case '\n': buf_puts(&b, "\\n"); break;
		case '\r': buf_puts(&b, "\\r"); break;
		case '\t': buf_puts(&b, "\\t"); break;
		default:
			if (c < 0x20 || c == 0x7f)
				buf_appendf(&b, "\\x%02x", c);
			else
				buf_append(&b, (const char *)&c, 1);
		}
	}
	return buf_steal(&b);
}

void log_close(void)
{
	if (out)
		fclose(out);
	out = NULL;
}
