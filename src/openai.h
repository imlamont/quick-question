#ifndef QQ_OPENAI_H
#define QQ_OPENAI_H

#include "config.h"

#include <stddef.h>

/* endpoint + "/chat/completions" unless already present. malloc'd. */
char *openai_url(const char *endpoint);

/* POST a chat completion, running the tool calls the model makes (local tools
 * enabled by the TOOLS_* flags in tools, and MCP tools) for up to
 * QQ_MAX_TOOL_ROUNDS rounds within one overall timeout. Time spent at approval
 * prompts doesn't count. Requires curl_global_init(). Returns the malloc'd
 * final reply content, or NULL with a message in err. */
char *openai_ask(const struct profile *p, const char *system, const char *user,
		 int tools, long timeout, char *err, size_t errlen);

#endif
