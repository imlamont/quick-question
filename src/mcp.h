#ifndef QQ_MCP_H
#define QQ_MCP_H

/* Tool calling through a LiteLLM proxy's MCP gateway.
 *
 * A profile's "mcp_servers" are offered to the model as {"type": "mcp"} tools.
 * The proxy runs the first round of tool calls itself and hands any further
 * ones back; qq runs those via POST /mcp-rest/tools/call. The model sees tools
 * as "<server>-<tool>", which is how a call is mapped back to its server. */

#include "http.h"

struct cJSON;

/* Add a "tools" array offering every server in servers (JSON array of names). */
void mcp_add_tools(struct cJSON *req, const struct cJSON *servers);

/* The proxy's tool-call URL, derived from the chat endpoint by dropping
 * /chat/completions and /v1. malloc'd. */
char *mcp_call_url(const char *endpoint);

/* The server whose "<server>-" prefix names this tool (longest wins), with the
 * bare tool name in *tool; NULL if none matches. */
const char *mcp_match(const struct cJSON *servers, const char *name, const char **tool);

/* Text of an MCP CallToolResult: its text items joined by newlines, else its
 * structuredContent as JSON. malloc'd, never NULL. */
char *mcp_result_text(const struct cJSON *result);

/* Run one assistant tool_call. Always returns malloc'd text for the tool
 * message; failures are described as "error: ..." so the model can react. */
char *mcp_call(struct http *c, const char *url, const struct cJSON *servers,
	       const struct cJSON *tool_call);

#endif
