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

/* Run one assistant tool_call for a local tool. Time spent waiting for the
 * user's approval is added to *waited_ms; commands are killed at deadline_ms
 * plus that time. Always returns malloc'd text for the tool message, starting
 * with "error: " when nothing was done. */
char *tools_call(const struct cJSON *tool_call, int enabled, long long deadline_ms,
		 long long *waited_ms);

/* 1 if path resolves inside the working directory, following symlinks. A path
 * that doesn't exist yet is judged by its parent directory. */
int tools_inside_cwd(const char *path);

/* text with its single occurrence of old_text replaced by new_text (malloc'd).
 * Returns NULL, with *count set to the number of occurrences, unless there is
 * exactly one. */
char *tools_replace_once(const char *text, const char *old_text, const char *new_text,
			 size_t *count);

#endif
