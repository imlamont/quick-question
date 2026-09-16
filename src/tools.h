#ifndef QQ_TOOLS_H
#define QQ_TOOLS_H

/* Local tools the model may use, enabled with -r, -w and -x.
 *
 * qq defines these tools for the model and runs them itself. Reads inside the
 * working directory run directly; writes, edits, commands and reads outside
 * it are confirmed on the terminal first. */

#include "buf.h"

struct cJSON;

enum {
	TOOLS_READ = 1,  /* -r: read_file, list_directory, search_files */
	TOOLS_WRITE = 2, /* -w: write_file, edit_file */
	TOOLS_EXEC = 4,  /* -x: run_command */
};

/* Add OpenAI function definitions for the enabled tools to req's "tools"
 * array, creating it if needed. */
void tools_add_defs(struct cJSON *req, int enabled);

/* System-prompt text telling the model it has tools and how approval works. */
void tools_describe(struct buf *out, int enabled);

/* The TOOLS_* flag that enables tool name, or 0 if it isn't a local tool. */
int tools_flag(const char *name);

enum { TOOLS_ANSWER_NO = 0, TOOLS_ANSWER_YES = 1, TOOLS_ANSWER_TIMEOUT = -2 };

/* Read one y/N answer from fd, waiting at most timeout_ms. End of input counts
 * as no, so an unattended prompt can't wait forever. */
int tools_read_answer(int fd, long long timeout_ms);

/* Run one assistant tool_call for a local tool. Time spent waiting for the
 * user's approval is added to *waited_ms; commands are killed at deadline_ms
 * plus that time. *refused is set when an approval prompt was answered no,
 * timed out, or could not be shown, so the caller can reuse that answer rather
 * than ask again. Always returns malloc'd text for the tool message, starting
 * with "error: " when nothing was done. */
char *tools_call(const struct cJSON *tool_call, int enabled, long long deadline_ms,
		 long long *waited_ms, int *refused);

/* 1 if path resolves inside the working directory, following symlinks. A path
 * that doesn't exist yet is judged by its parent directory. */
int tools_inside_cwd(const char *path);

/* text with its single occurrence of old_text replaced by new_text (malloc'd).
 * Returns NULL, with *count set to the number of occurrences, unless there is
 * exactly one. */
char *tools_replace_once(const char *text, const char *old_text, const char *new_text,
			 size_t *count);

#endif
