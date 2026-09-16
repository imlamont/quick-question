#include "openai.h"
#include "buf.h"
#include "http.h"
#include "log.h"
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

/* Local tool calls already answered, so a model that repeats one can't make qq
 * ask the user the same question twice, or spin on a call that changes nothing.
 * Reads are marked, because anything that alters the machine makes them stale. */
struct history {
	char **sig;
	char **result;
	int *is_read;
	size_t n, cap;
};

/* "name\x1farguments" for a tool call, or NULL if it has no name. Models that
 * loop repeat their own output, so comparing the arguments as they arrive is
 * enough; qq doesn't try to normalize equivalent JSON. */
static char *call_sig(const cJSON *tool_call)
{
	const cJSON *fn = cJSON_GetObjectItemCaseSensitive(tool_call, "function");
	const cJSON *name = cJSON_GetObjectItemCaseSensitive(fn, "name");
	const cJSON *args = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
	struct buf b = {0};

	if (!cJSON_IsString(name))
		return NULL;
	buf_appendf(&b, "%s\x1f", name->valuestring);
	if (cJSON_IsString(args)) {
		buf_puts(&b, args->valuestring);
	} else if (args) {
		char *text = cJSON_PrintUnformatted(args);

		if (text) {
			buf_puts(&b, text);
			cJSON_free(text);
		}
	}
	return buf_steal(&b);
}

static const char *history_find(const struct history *h, const char *sig)
{
	for (size_t i = 0; i < h->n; i++)
		if (!strcmp(h->sig[i], sig))
			return h->result[i];
	return NULL;
}

static void history_add(struct history *h, const char *sig, const char *result, int is_read)
{
	if (h->n == h->cap) {
		h->cap = h->cap ? h->cap * 2 : 8;
		h->sig = realloc(h->sig, h->cap * sizeof *h->sig);
		h->result = realloc(h->result, h->cap * sizeof *h->result);
		h->is_read = realloc(h->is_read, h->cap * sizeof *h->is_read);
		if (!h->sig || !h->result || !h->is_read) {
			fputs("qq: out of memory\n", stderr);
			exit(1);
		}
	}
	if (!(h->sig[h->n] = strdup(sig)) || !(h->result[h->n] = strdup(result))) {
		fputs("qq: out of memory\n", stderr);
		exit(1);
	}
	h->is_read[h->n] = is_read;
	h->n++;
}

/* Drop the remembered reads, so a read that follows a change is really done
 * again. Called whenever a write, edit or command was carried out. */
static void history_forget_reads(struct history *h)
{
	size_t kept = 0;

	for (size_t i = 0; i < h->n; i++) {
		if (h->is_read[i]) {
			free(h->sig[i]);
			free(h->result[i]);
			continue;
		}
		h->sig[kept] = h->sig[i];
		h->result[kept] = h->result[i];
		h->is_read[kept] = h->is_read[i];
		kept++;
	}
	h->n = kept;
}

static void history_free(struct history *h)
{
	for (size_t i = 0; i < h->n; i++) {
		free(h->sig[i]);
		free(h->result[i]);
	}
	free(h->sig);
	free(h->result);
	free(h->is_read);
	*h = (struct history){0};
}

/* The result for a repeated call: what happened the first time, and a note that
 * qq did not do it again. */
static char *replay(const char *name, const char *earlier)
{
	struct buf b = {0};

	buf_appendf(&b, "error: you already called %s with exactly these arguments, so qq did "
		       "not do it again. The earlier result was:\n%s", name, earlier);
	return buf_steal(&b);
}

/* Append the assistant's tool-call turn, then one tool message per call.
 * Local tools run here; anything else goes to the MCP gateway. Returns the
 * number of calls actually carried out, so a round of nothing but repeats can
 * be told apart from progress. */
static int run_tools(struct http *c, const char *url, const struct profile *p, int tools,
		     cJSON *messages, const cJSON *tool_calls, struct history *h, int turn)
{
	cJSON *assistant = cJSON_CreateObject();
	const cJSON *tc;
	int done = 0;

	cJSON_AddStringToObject(assistant, "role", "assistant");
	cJSON_AddNullToObject(assistant, "content");
	cJSON_AddItemToObject(assistant, "tool_calls", cJSON_Duplicate(tool_calls, 1));
	cJSON_AddItemToArray(messages, assistant);

	log_printf("tool-round %d calls=%d", turn, cJSON_GetArraySize(tool_calls));
	cJSON_ArrayForEach(tc, tool_calls) {
		const cJSON *id = cJSON_GetObjectItemCaseSensitive(tc, "id");
		const cJSON *fn = cJSON_GetObjectItemCaseSensitive(tc, "function");
		const cJSON *name = cJSON_GetObjectItemCaseSensitive(fn, "name");
		long long waited_ms = 0;
		char *result, *sig = NULL;
		const char *earlier;
		cJSON *m;

		int flag = cJSON_IsString(name) ? tools_flag(name->valuestring) : 0;
		int refused = 0;

		/* Local and MCP calls are remembered alike: a model that repeats
		 * itself is answered from the record either way, whether that
		 * spares the user a second approval or the search engine a
		 * second identical query. */
		sig = call_sig(tc);
		if (sig && (earlier = history_find(h, sig))) {
			log_printf("tool-repeat %s answered from history", name->valuestring);
			result = replay(name->valuestring, earlier);
		} else {
			if (flag) {
				result = tools_call(tc, tools, c->deadline_ms, &waited_ms,
						    &refused);
				/* A write, edit or command that actually ran invalidates
				 * every remembered read. */
				if (!refused && flag != TOOLS_READ)
					history_forget_reads(h);
			} else {
				result = mcp_call(c, url, p->mcp_servers, tc);
			}
			done++;
			/* Only a local read goes stale when the machine changes; an
			 * MCP result is not ours to second-guess. */
			if (sig)
				history_add(h, sig, result, !refused && flag == TOOLS_READ);
		}
		free(sig);
		/* Time spent at an approval prompt doesn't count against -t. */
		c->deadline_ms += waited_ms;

		m = cJSON_CreateObject();
		cJSON_AddStringToObject(m, "role", "tool");
		cJSON_AddStringToObject(m, "tool_call_id", cJSON_IsString(id) ? id->valuestring : "");
		cJSON_AddStringToObject(m, "content", result);
		cJSON_AddItemToArray(messages, m);
		free(result);
	}
	return done;
}

char *openai_ask(const struct profile *p, const char *system, const char *user,
		 int tools, long timeout, char *err, size_t errlen)
{
	const char *key = p->api_key_env ? getenv(p->api_key_env) : NULL;
	char *chat_url = openai_url(p->endpoint), *tool_url = mcp_call_url(p->endpoint);
	char *reply = NULL;
	cJSON *req = new_request(p, system, user, tools), *res = NULL;
	const cJSON *msg, *content, *tool_calls;
	struct history h = {0};
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
		if (!run_tools(&c, tool_url, p, tools,
			       cJSON_GetObjectItemCaseSensitive(req, "messages"), tool_calls, &h,
			       turn)) {
			snprintf(err, errlen, "the model kept repeating tool calls it had already "
					      "made, so qq stopped");
			goto out;
		}
	}
out:
	history_free(&h);
	cJSON_Delete(res);
	cJSON_Delete(req);
	http_free(&c);
	buf_free(&resp);
	free(chat_url);
	free(tool_url);
	return reply;
}
