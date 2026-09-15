#include "mcp.h"
#include "buf.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mcp_add_tools(cJSON *req, const cJSON *servers)
{
	cJSON *tools = cJSON_AddArrayToObject(req, "tools");
	const cJSON *s;

	cJSON_ArrayForEach(s, servers) {
		cJSON *t = cJSON_CreateObject();
		struct buf url = {0};

		buf_appendf(&url, "litellm_proxy/mcp/%s", s->valuestring);
		cJSON_AddStringToObject(t, "type", "mcp");
		cJSON_AddStringToObject(t, "server_label", s->valuestring);
		cJSON_AddStringToObject(t, "server_url", url.data);
		cJSON_AddStringToObject(t, "require_approval", "never");
		cJSON_AddItemToArray(tools, t);
		buf_free(&url);
	}
}

/* Length of s[0..n) without trailing slashes and then suffix, if present. */
static size_t strip_suffix(const char *s, size_t n, const char *suffix)
{
	size_t sl = strlen(suffix);

	while (n && s[n - 1] == '/')
		n--;
	if (n >= sl && !memcmp(s + n - sl, suffix, sl))
		n -= sl;
	while (n && s[n - 1] == '/')
		n--;
	return n;
}

char *mcp_call_url(const char *endpoint)
{
	size_t n = strip_suffix(endpoint, strlen(endpoint), "/chat/completions");
	struct buf b = {0};

	n = strip_suffix(endpoint, n, "/v1");
	buf_append(&b, endpoint, n);
	buf_puts(&b, "/mcp-rest/tools/call");
	return buf_steal(&b);
}

const char *mcp_match(const cJSON *servers, const char *name, const char **tool)
{
	const char *best = NULL;
	size_t best_len = 0;
	const cJSON *s;

	cJSON_ArrayForEach(s, servers) {
		size_t n = strlen(s->valuestring);

		if (n > best_len && !strncmp(name, s->valuestring, n) && name[n] == '-' && name[n + 1]) {
			best = s->valuestring;
			best_len = n;
		}
	}
	if (best)
		*tool = name + best_len + 1;
	return best;
}

char *mcp_result_text(const cJSON *result)
{
	const cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
	const cJSON *structured = cJSON_GetObjectItemCaseSensitive(result, "structuredContent");
	const cJSON *item;
	struct buf b = {0};

	cJSON_ArrayForEach(item, content) {
		const cJSON *text = cJSON_GetObjectItemCaseSensitive(item, "text");

		if (cJSON_IsString(text)) {
			if (b.len)
				buf_puts(&b, "\n");
			buf_puts(&b, text->valuestring);
		}
	}
	if (!b.len && cJSON_IsObject(structured)) {
		char *s = cJSON_PrintUnformatted(structured);

		if (s)
			buf_puts(&b, s);
		cJSON_free(s);
	}
	return buf_steal(&b);
}

char *mcp_call(struct http *c, const char *url, const cJSON *servers, const cJSON *tool_call)
{
	const cJSON *fn = cJSON_GetObjectItemCaseSensitive(tool_call, "function");
	const cJSON *name = cJSON_GetObjectItemCaseSensitive(fn, "name");
	const cJSON *args = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
	const char *server, *tool;
	cJSON *arguments, *req = NULL, *res = NULL;
	struct buf resp = {0}, out = {0};
	char err[512], *text;
	long status;

	if (!cJSON_IsString(name) || !(server = mcp_match(servers, name->valuestring, &tool))) {
		buf_appendf(&out, "error: unknown tool \"%s\"",
			    cJSON_IsString(name) ? name->valuestring : "(unnamed)");
		return buf_steal(&out);
	}

	/* Arguments normally arrive JSON-encoded in a string; "" means none. */
	if (cJSON_IsString(args))
		arguments = *args->valuestring ? cJSON_Parse(args->valuestring) : cJSON_CreateObject();
	else
		arguments = cJSON_Duplicate(args, 1);
	if (!cJSON_IsObject(arguments)) {
		cJSON_Delete(arguments);
		buf_puts(&out, "error: tool arguments are not a JSON object");
		return buf_steal(&out);
	}

	req = cJSON_CreateObject();
	cJSON_AddStringToObject(req, "server_id", server);
	cJSON_AddStringToObject(req, "name", tool);
	if (!cJSON_AddItemToObject(req, "arguments", arguments))
		cJSON_Delete(arguments);

	if (http_post_json(c, url, req, &status, &resp, &res, err, sizeof err)) {
		buf_appendf(&out, "error: %s", err);
	} else if (status >= 400) {
		buf_appendf(&out, "error: HTTP %ld: %.300s", status, http_error_message(res, &resp));
	} else {
		if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(res, "isError")))
			buf_puts(&out, "error: ");
		text = mcp_result_text(res);
		buf_puts(&out, text);
		free(text);
	}

	cJSON_Delete(req);
	cJSON_Delete(res);
	buf_free(&resp);
	return buf_steal(&out);
}
