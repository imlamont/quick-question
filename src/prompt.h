#ifndef QQ_PROMPT_H
#define QQ_PROMPT_H

#include "buf.h"

#include <stddef.h>

/* Built-in steering that always leads the system prompt. */
extern const char prompt_steer[];

/* Join the non-empty parts into out (expected empty) with blank lines. */
void prompt_system(struct buf *out, const char *const parts[], size_t n);

/* "Environment: OS=..., shell=..., cwd=..." for -c. */
void prompt_context(struct buf *out);

/* The prompt, with piped input appended in <stdin> tags when both exist. */
void prompt_user(struct buf *out, const char *prompt, const char *input, size_t input_len);

/* Strip a leading <think>...</think> block and surrounding whitespace in place. */
char *prompt_clean(char *reply);

#endif
