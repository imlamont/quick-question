#include "openai.h"
#include "buf.h"
#include "http.h"
#include "mcp.h"
#include "qq.h"
#include "tools.h"

#include <cjson/cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *openai_url(const char *endpoint)
{
	static const char suffix[] = "/chat/completions";
	const size_t sl = sizeof suffix - 1;
	size_t n = strlen(endpoint);
	struct buf b = {0};

	while (n && endpoint[n - 1] == '/')
		n--;
	buf_append(&b, endpoint, n);
	if (n < sl || memcmp(endpoint + n - sl, suffix, sl))
		buf_append(&b, suffix, sl);
	return buf_steal(&b);
}

static void add_message(cJSON *messages, const char *role, const char *content)
{
	cJSON *m = cJSON_CreateObject();

	cJSON_AddStringToObject(m, "role", role);
	cJSON_AddStringToObject(m, "content", content);
	cJSON_AddItemToArray(messages, m);
}

static cJSON *new_request(const struct profile *p, const char *system, const char *user,
			  int tools)
{
	cJSON *req = cJSON_CreateObject(), *messages;

	cJSON_AddStringToObject(req, "model", p->model);
	messages = cJSON_AddArrayToObject(req, "messages");
	add_message(messages, "system", system);
	add_message(messages, "user", user);
	cJSON_AddFalseToObject(req, "stream");
	if (!isnan(p->temperature))
		cJSON_AddNumberToObject(req, "temperature", p->temperature);
	if (p->max_tokens > 0)
		cJSON_AddNumberToObject(req, "max_tokens", p->max_tokens);
	if (p->mcp_servers)
		mcp_add_tools(req, p->mcp_servers);
	tools_add_defs(req, tools);
	return req;
}

/* Append the assistant's tool-call turn, then one tool message per call.
 * Local tools run here; anything else goes to the MCP gateway. */
static void run_tools(struct http *c, const char *url, const struct profile *p, int tools,
		      cJSON *messages, const cJSON *tool_calls)
{
	cJSON *assistant = cJSON_CreateObject();
	const cJSON *tc;

	cJSON_AddStringToObject(assistant, "role", "assistant");
	cJSON_AddNullToObject(assistant, "content");
	cJSON_AddItemToObject(assistant, "tool_calls", cJSON_Duplicate(tool_calls, 1));
	cJSON_AddItemToArray(messages, assistant);

	cJSON_ArrayForEach(tc, tool_calls) {
		const cJSON *id = cJSON_GetObjectItemCaseSensitive(tc, "id");
		const cJSON *fn = cJSON_GetObjectItemCaseSensitive(tc, "function");
		const cJSON *name = cJSON_GetObjectItemCaseSensitive(fn, "name");
		long long waited_ms = 0;
		char *result;
		cJSON *m;

		if (cJSON_IsString(name) && tools_flag(name->valuestring))
			result = tools_call(tc, tools, c->deadline_ms, &waited_ms);
		else
			result = mcp_call(c, url, p->mcp_servers, tc);
		/* Time spent at an approval prompt doesn't count against -t. */
		c->deadline_ms += waited_ms;

		m = cJSON_CreateObject();
		cJSON_AddStringToObject(m, "role", "tool");
		cJSON_AddStringToObject(m, "tool_call_id", cJSON_IsString(id) ? id->valuestring : "");
		cJSON_AddStringToObject(m, "content", result);
		cJSON_AddItemToArray(messages, m);
		free(result);
	}
}

char *openai_ask(const struct profile *p, const char *system, const char *user,
		 int tools, long timeout, char *err, size_t errlen)
{
	const char *key = p->api_key_env ? getenv(p->api_key_env) : NULL;
	char *chat_url = openai_url(p->endpoint), *tool_url = mcp_call_url(p->endpoint);
	char *reply = NULL;
	cJSON *req = new_request(p, system, user, tools), *res = NULL;
	const cJSON *msg, *content, *tool_calls;
	struct buf resp = {0};
	struct http c;
	long status;

	if (http_init(&c, key, timeout, err, errlen))
		goto out;

	/* Ask; while the model answers with tool calls, run them and ask again. */
	for (int turn = 0;; turn++) {
		cJSON_Delete(res);
		if (http_post_json(&c, chat_url, req, &status, &resp, &res, err, errlen))
			goto out;
		if (status >= 400) {
			snprintf(err, errlen, "HTTP %ld: %.300s", status, http_error_message(res, &resp));
			goto out;
		}

		msg = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(res, "choices"), 0);
		msg = cJSON_GetObjectItemCaseSensitive(msg, "message");
		content = cJSON_GetObjectItemCaseSensitive(msg, "content");
		tool_calls = cJSON_GetObjectItemCaseSensitive(msg, "tool_calls");

		if (!cJSON_IsArray(tool_calls) || cJSON_GetArraySize(tool_calls) == 0) {
			if (!cJSON_IsString(content))
				snprintf(err, errlen, "unexpected response: %.200s",
					 resp.data ? resp.data : "(empty body)");
			else if (!(reply = strdup(content->valuestring)))
				snprintf(err, errlen, "out of memory");
			goto out;
		}
		if (turn == QQ_MAX_TOOL_ROUNDS) {
			snprintf(err, errlen, "model still calling tools after %d rounds",
				 QQ_MAX_TOOL_ROUNDS);
			goto out;
		}
		run_tools(&c, tool_url, p, tools, cJSON_GetObjectItemCaseSensitive(req, "messages"),
			  tool_calls);
	}
out:
	cJSON_Delete(res);
	cJSON_Delete(req);
	http_free(&c);
	buf_free(&resp);
	free(chat_url);
	free(tool_url);
	return reply;
}
