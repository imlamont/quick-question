/* Unit tests for buf, prompt, config parsing, URL joining, MCP and local tool helpers. */
#include "buf.h"
#include "config.h"
#include "log.h"
#include "mcp.h"
#include "openai.h"
#include "prompt.h"
#include "tools.h"

#include <cjson/cJSON.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks, failures;

#define CHECK(cond)                                                              \
	do {                                                                     \
		checks++;                                                        \
		if (!(cond)) {                                                   \
			failures++;                                              \
			fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
		}                                                                \
	} while (0)

#define STREQ(got, want)                                                         \
	do {                                                                     \
		const char *g_ = (got), *w_ = (want);                            \
		checks++;                                                        \
		if (!g_ || strcmp(g_, w_)) {                                     \
			failures++;                                              \
			fprintf(stderr, "%s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, \
				g_ ? g_ : "(null)", w_);                          \
		}                                                                \
	} while (0)

static void test_buf(void)
{
	struct buf b = {0};
	char *s, trim[] = "  \t hi there \n\n";

	for (int i = 0; i < 10000; i++)
		buf_append(&b, "x", 1);
	CHECK(b.len == 10000 && strlen(b.data) == 10000);
	buf_free(&b);
	CHECK(!b.data && !b.len);

	buf_appendf(&b, "%s=%d", "n", 42);
	buf_puts(&b, "!");
	STREQ(b.data, "n=42!");
	s = buf_steal(&b);
	STREQ(s, "n=42!");
	CHECK(!b.data);
	free(s);

	s = buf_steal(&b);
	STREQ(s, "");
	free(s);

	/* Reserving without writing (e.g. a read that hits EOF) must still
	 * leave a valid empty string. */
	buf_reserve(&b, 65536);
	STREQ(b.data, "");
	CHECK(b.len == 0);
	buf_free(&b);

	STREQ(str_trim(trim), "hi there");
}

static void test_system(void)
{
	struct buf b = {0};
	const char *parts[] = { "A", NULL, "", "B", "C" };

	prompt_system(&b, parts, 5);
	STREQ(b.data, "A\n\nB\n\nC");
	buf_free(&b);
}

static void test_user(void)
{
	struct buf b = {0};

	prompt_user(&b, "explain", "data\n", 5);
	STREQ(b.data, "explain\n\n<stdin>\ndata\n</stdin>");
	buf_free(&b);

	prompt_user(&b, "explain", "data", 4);
	STREQ(b.data, "explain\n\n<stdin>\ndata\n</stdin>");
	buf_free(&b);

	prompt_user(&b, "just a prompt", NULL, 0);
	STREQ(b.data, "just a prompt");
	buf_free(&b);

	prompt_user(&b, NULL, "only stdin", 10);
	STREQ(b.data, "only stdin");
	buf_free(&b);
}

static void test_clean(void)
{
	char a[] = "  <think>\nhmm\n</think>\n\n hello \n";
	char b[] = "<think>never closed";
	char c[] = "\n plain answer\n";
	char d[] = "keep <think>inline</think> tags";

	STREQ(prompt_clean(a), "hello");
	STREQ(prompt_clean(b), "<think>never closed");
	STREQ(prompt_clean(c), "plain answer");
	STREQ(prompt_clean(d), "keep <think>inline</think> tags");
}

static void test_url(void)
{
	const char *cases[][2] = {
		{ "http://h/v1", "http://h/v1/chat/completions" },
		{ "http://h/v1///", "http://h/v1/chat/completions" },
		{ "http://h/v1/chat/completions", "http://h/v1/chat/completions" },
		{ "http://h/v1/chat/completions/", "http://h/v1/chat/completions" },
	};

	for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
		char *u = openai_url(cases[i][0]);

		STREQ(u, cases[i][1]);
		free(u);
	}
}

static void test_mcp(void)
{
	const char *urls[][2] = {
		{ "http://h:4000", "http://h:4000/mcp-rest/tools/call" },
		{ "http://h:4000/", "http://h:4000/mcp-rest/tools/call" },
		{ "http://h:4000/v1/", "http://h:4000/mcp-rest/tools/call" },
		{ "http://h:4000/v1/chat/completions", "http://h:4000/mcp-rest/tools/call" },
		{ "http://h/proxy/v1", "http://h/proxy/mcp-rest/tools/call" },
	};
	cJSON *servers = cJSON_Parse("[\"searx\", \"searx-news\", \"db\"]");
	cJSON *one = cJSON_Parse("[\"db\"]");
	cJSON *req = cJSON_CreateObject(), *result;
	const char *tool = NULL;
	char *s;

	for (size_t i = 0; i < sizeof urls / sizeof *urls; i++) {
		s = mcp_call_url(urls[i][0]);
		STREQ(s, urls[i][1]);
		free(s);
	}

	STREQ(mcp_match(servers, "searx-web_search", &tool), "searx");
	STREQ(tool, "web_search");
	STREQ(mcp_match(servers, "searx-news-latest", &tool), "searx-news"); /* longest prefix wins */
	STREQ(tool, "latest");
	CHECK(!mcp_match(servers, "other-web_search", &tool));
	CHECK(!mcp_match(servers, "searx-", &tool));
	CHECK(!mcp_match(servers, "searxweb", &tool));
	CHECK(!mcp_match(NULL, "searx-web", &tool));

	result = cJSON_Parse("{\"content\":[{\"type\":\"text\",\"text\":\"a\"},{\"type\":\"image\"},"
			     "{\"type\":\"text\",\"text\":\"b\"}],\"structuredContent\":{\"r\":1}}");
	s = mcp_result_text(result);
	STREQ(s, "a\nb");
	free(s);
	cJSON_Delete(result);

	result = cJSON_Parse("{\"content\":[],\"structuredContent\":{\"result\":\"x\"}}");
	s = mcp_result_text(result);
	STREQ(s, "{\"result\":\"x\"}");
	free(s);
	cJSON_Delete(result);

	s = mcp_result_text(NULL);
	STREQ(s, "");
	free(s);

	mcp_add_tools(req, one);
	s = cJSON_PrintUnformatted(req);
	STREQ(s, "{\"tools\":[{\"type\":\"mcp\",\"server_label\":\"db\","
		 "\"server_url\":\"litellm_proxy/mcp/db\",\"require_approval\":\"always\"}]}");
	cJSON_free(s);

	cJSON_Delete(req);
	cJSON_Delete(one);
	cJSON_Delete(servers);
}

/* Comma-separated function names in req's "tools" array. */
static void tool_names(const cJSON *req, struct buf *out)
{
	const cJSON *tools = cJSON_GetObjectItemCaseSensitive(req, "tools"), *tool;

	cJSON_ArrayForEach(tool, tools) {
		const cJSON *fn = cJSON_GetObjectItemCaseSensitive(tool, "function");
		const cJSON *name = cJSON_GetObjectItemCaseSensitive(fn, "name");

		buf_appendf(out, "%s%s", out->len ? "," : "", cJSON_IsString(name) ? name->valuestring : "?");
	}
}

static void test_tools(void)
{
	char dir[] = "/tmp/qq-unit-XXXXXX", *s;
	cJSON *req, *servers = cJSON_Parse("[\"db\"]");
	struct buf b = {0};
	size_t count;
	FILE *f;
	int here;

	s = tools_replace_once("a b a", "b", "B", &count);
	STREQ(s, "a B a");
	CHECK(count == 1);
	free(s);
	CHECK(!tools_replace_once("a b a", "a", "A", &count) && count == 2);
	CHECK(!tools_replace_once("abc", "x", "y", &count) && count == 0);
	CHECK(!tools_replace_once("abc", "", "y", &count));

	CHECK(tools_flag("read_file") == TOOLS_READ);
	CHECK(tools_flag("search_files") == TOOLS_READ);
	CHECK(tools_flag("edit_file") == TOOLS_WRITE);
	CHECK(tools_flag("run_command") == TOOLS_EXEC);
	CHECK(tools_flag("searxng_mcp-web_search") == 0);

	tools_describe(&b, TOOLS_READ);
	CHECK(b.data && strstr(b.data, "The user approves any read outside the current directory;"));
	buf_free(&b);
	tools_describe(&b, TOOLS_EXEC);
	CHECK(b.data && strstr(b.data, "approves each file change and command, and any read outside"));
	buf_free(&b);
	tools_describe(&b, 0);
	CHECK(!b.data);

	req = cJSON_CreateObject();
	tools_add_defs(req, 0);
	CHECK(!cJSON_GetObjectItemCaseSensitive(req, "tools"));
	tools_add_defs(req, TOOLS_WRITE | TOOLS_EXEC);
	tool_names(req, &b);
	STREQ(b.data, "write_file,edit_file,run_command");
	buf_free(&b);
	s = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(
		cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(
			cJSON_GetObjectItemCaseSensitive(req, "tools"), 2), "function"), "parameters"));
	STREQ(s, "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},"
		 "\"required\":[\"command\"]}");
	cJSON_free(s);
	cJSON_Delete(req);

	/* Local tools share the "tools" array with MCP servers. */
	req = cJSON_CreateObject();
	mcp_add_tools(req, servers);
	tools_add_defs(req, TOOLS_READ);
	CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(req, "tools")) == 4);
	CHECK(cJSON_GetArraySize(req) == 1);
	cJSON_Delete(req);
	cJSON_Delete(servers);

	/* tools_read_answer: y/yes approve, anything else (or silence) doesn't */
	{
		const struct { const char *typed; int want; } answers[] = {
			{ "y\n", TOOLS_ANSWER_YES },   { "yes\n", TOOLS_ANSWER_YES },
			{ " Y \n", TOOLS_ANSWER_YES }, { "YES\n", TOOLS_ANSWER_YES },
			{ "n\n", TOOLS_ANSWER_NO },    { "\n", TOOLS_ANSWER_NO },
			{ "later\n", TOOLS_ANSWER_NO },
		};
		int p[2];

		for (size_t i = 0; i < sizeof answers / sizeof *answers; i++) {
			CHECK(pipe(p) == 0);
			CHECK(write(p[1], answers[i].typed, strlen(answers[i].typed)) > 0);
			CHECK(tools_read_answer(p[0], 1000) == answers[i].want);
			close(p[0]);
			close(p[1]);
		}

		CHECK(pipe(p) == 0); /* nothing typed: times out, which is not approval */
		CHECK(tools_read_answer(p[0], 50) == TOOLS_ANSWER_TIMEOUT);
		close(p[1]); /* end of input: also not approval */
		CHECK(tools_read_answer(p[0], 1000) == TOOLS_ANSWER_NO);
		close(p[0]);
	}

	/* tools_inside_cwd, from inside a scratch directory */
	here = open(".", O_RDONLY);
	CHECK(here >= 0 && mkdtemp(dir) && chdir(dir) == 0);
	CHECK(mkdir("sub", 0700) == 0);
	if ((f = fopen("sub/file", "w")))
		fclose(f);
	CHECK(symlink("/", "escape") == 0);

	CHECK(tools_inside_cwd("."));
	CHECK(tools_inside_cwd("sub/file"));
	CHECK(tools_inside_cwd("sub/../sub/file"));
	CHECK(tools_inside_cwd("new.txt"));      /* doesn't exist yet */
	CHECK(tools_inside_cwd("sub/new.txt"));
	CHECK(!tools_inside_cwd(".."));
	CHECK(!tools_inside_cwd("../elsewhere"));
	CHECK(!tools_inside_cwd("/etc/passwd"));
	CHECK(!tools_inside_cwd("escape"));      /* symlink out of the tree */
	CHECK(!tools_inside_cwd("escape/etc"));
	CHECK(!tools_inside_cwd("missing/deep/x"));

	unlink("escape");
	unlink("sub/file");
	rmdir("sub");
	CHECK(fchdir(here) == 0);
	rmdir(dir);
	close(here);
}

static const char sample[] =
	"{\"default\":\"local/m\",\"system_prompt\":\"global\",\"timeout\":90,\"gateways\":{"
	"\"local\":{\"endpoint\":\"http://h/v1\",\"api_key_env\":\"KEY\",\"system_prompt\":\"gw\","
	"\"temperature\":0.5,\"max_tokens\":64,\"timeout\":60,\"tools\":[\"read\",\"write\"],"
	"\"mcp_servers\":[\"searx\",\"other\"],\"allow_danger\":true,\"models\":{"
	"\"m\":{\"id\":\"real-m\"},"
	"\"narrow\":{\"tools\":[\"read\"],\"mcp_servers\":[\"searx\"],\"system_prompt\":\"model\","
	"\"temperature\":0.9,\"max_tokens\":8,\"timeout\":5,\"allow_danger\":false},"
	"\"none\":{\"tools\":[],\"mcp_servers\":[]},"
	"\"meta/llama\":{}}},"
	"\"plain\":{\"endpoint\":\"http://p\",\"api_key_env\":\"\",\"default_model\":\"m2\","
	"\"models\":{\"m\":{},\"m2\":{}}}}}";

/* One gateway "g" at http://h with the given members, and a model "m" with
 * the given members; either may be empty. */
static void gw_json(char *buf, size_t n, const char *gateway, const char *model)
{
	snprintf(buf, n,
		 "{\"gateways\":{\"g\":{\"endpoint\":\"http://h\"%s%s,\"models\":{\"m\":{%s}}}}}",
		 *gateway ? "," : "", gateway, model);
}

/* Parse json and resolve sel; returns the result and leaves the message in err. */
static int try_select(const char *json, const char *sel, char *err, size_t errlen)
{
	struct config c = {0};
	struct profile p;
	int r = config_parse(&c, json, strlen(json), err, errlen);

	if (!r)
		r = config_profile(&c, sel, &p, err, errlen);
	config_free(&c);
	return r;
}

/* Parse a one-model config built by gw_json. */
static int try_gw(const char *gateway, const char *model, char *err, size_t errlen)
{
	char json[1024];

	gw_json(json, sizeof json, gateway, model);
	return try_select(json, "g", err, errlen);
}

static void test_log(void)
{
	char *s;

	/* Nothing an entry carries may break the line or steer a terminal. */
	s = log_escape("plain", 5);
	STREQ(s, "plain");
	free(s);
	s = log_escape("a\nb\tc\rd", 7);
	STREQ(s, "a\\nb\\tc\\rd");
	free(s);
	s = log_escape("back\\slash", 10);
	STREQ(s, "back\\\\slash");
	free(s);
	s = log_escape("\033[2Jbell\a", 9);
	STREQ(s, "\\x1b[2Jbell\\x07");
	free(s);
	s = log_escape("del\177", 4);
	STREQ(s, "del\\x7f");
	free(s);
	/* Length is honoured, and a NUL byte is escaped rather than ending it. */
	s = log_escape("keep\0drop", 9);
	STREQ(s, "keep\\x00drop");
	free(s);
	s = log_escape(NULL, 0);
	STREQ(s, "");
	free(s);
	/* Writing with no log open is a no-op, not a crash. */
	CHECK(!log_on());
	log_printf("ignored %d", 1);
	log_json("ignored", NULL);
	log_command(1, (char *[]){ "qq" });
	log_close();

	/* The command line is written first, quoted the way a shell would need. */
	{
		char path[] = "/tmp/qq-unit-log-XXXXXX";
		char *args[] = { "qq", "-w", "two words", "don't", "tab\there", "" };
		char line[512] = "";
		int fd = mkstemp(path);
		FILE *f;

		CHECK(fd >= 0);
		close(fd);
		CHECK(log_open(path, line, sizeof line) == 0);
		CHECK(log_on());
		log_command(6, args);
		log_close();
		CHECK(!log_on());
		f = fopen(path, "r");
		CHECK(f && fgets(line, sizeof line, f));
		if (f)
			fclose(f);
		unlink(path);
		s = strstr(line, "command ");
		STREQ(s ? s : "", "command qq -w 'two words' 'don'\\''t' 'tab\\there' ''\n");
	}

}

static void test_config(void)
{
	struct config c = {0};
	struct profile p;
	struct buf l = {0};
	char err[512];

	setenv("KEY", "sk-unit", 1);
	CHECK(config_parse(&c, sample, strlen(sample), err, sizeof err) == 0);
	STREQ(c.default_sel, "local/m");
	STREQ(c.system_prompt, "global");
	CHECK(c.timeout == 90);

	/* The default, and everything a model inherits from its gateway. */
	CHECK(config_profile(&c, NULL, &p, err, sizeof err) == 0);
	STREQ(p.gateway, "local");
	STREQ(p.name, "m");
	STREQ(p.model, "real-m"); /* "id" is what the API is sent */
	STREQ(p.endpoint, "http://h/v1");
	STREQ(p.api_key_env, "KEY");
	STREQ(p.gateway_prompt, "gw");
	CHECK(!p.system_prompt);
	CHECK(p.temperature == 0.5 && p.max_tokens == 64 && p.timeout == 60);
	CHECK(p.tools == (TOOLS_READ | TOOLS_WRITE));
	CHECK(cJSON_GetArraySize(p.mcp_servers) == 2);
	CHECK(p.allow_danger == 1);

	/* A model narrows tools, servers and allow_danger, and overrides tuning. */
	CHECK(config_profile(&c, "local/narrow", &p, err, sizeof err) == 0);
	STREQ(p.model, "narrow"); /* no "id": the name is the id */
	CHECK(p.tools == TOOLS_READ);
	CHECK(cJSON_GetArraySize(p.mcp_servers) == 1);
	STREQ(cJSON_GetArrayItem(p.mcp_servers, 0)->valuestring, "searx");
	CHECK(p.allow_danger == 0);
	STREQ(p.gateway_prompt, "gw");
	STREQ(p.system_prompt, "model");
	CHECK(p.temperature == 0.9 && p.max_tokens == 8 && p.timeout == 5);

	/* [] is "none", not "inherit". */
	CHECK(config_profile(&c, "local/none", &p, err, sizeof err) == 0);
	CHECK(p.tools == 0 && !p.mcp_servers);

	/* A gateway with no timeout uses the top-level one. */
	CHECK(config_profile(&c, "plain", &p, err, sizeof err) == 0);
	STREQ(p.gateway, "plain");
	STREQ(p.name, "m2"); /* its "default_model" */
	CHECK(p.timeout == 90);
	CHECK(!p.api_key_env); /* "" is treated as unset */
	CHECK(isnan(p.temperature) && p.max_tokens == 0);
	CHECK(p.tools == 0 && !p.mcp_servers && p.allow_danger == 0);
	CHECK(!p.gateway_prompt && !p.system_prompt);

	/* Selecting: unique bare names, qualified names, gateway names. */
	CHECK(config_profile(&c, "narrow", &p, err, sizeof err) == 0);
	STREQ(p.gateway, "local");
	CHECK(config_profile(&c, "m2", &p, err, sizeof err) == 0);
	STREQ(p.gateway, "plain");
	CHECK(config_profile(&c, "plain/m", &p, err, sizeof err) == 0);
	STREQ(p.gateway, "plain");
	STREQ(p.name, "m");
	CHECK(config_profile(&c, "local", &p, err, sizeof err) == 0);
	STREQ(p.name, "m"); /* no "default_model": the first one listed */

	/* A model id with a "/" in it: bare, or after its gateway. */
	CHECK(config_profile(&c, "meta/llama", &p, err, sizeof err) == 0);
	STREQ(p.gateway, "local");
	STREQ(p.name, "meta/llama");
	CHECK(config_profile(&c, "local/meta/llama", &p, err, sizeof err) == 0);
	STREQ(p.name, "meta/llama");

	/* The same name on two gateways is never guessed. */
	CHECK(config_profile(&c, "m", &p, err, sizeof err) != 0);
	CHECK(strstr(err, "\"m\" is ambiguous; use gateway/model: local/m, plain/m"));

	CHECK(config_profile(&c, "nope", &p, err, sizeof err) != 0);
	CHECK(strstr(err, "unknown model or gateway \"nope\" (available: local/m, local/narrow, "
			  "local/none, local/meta/llama, plain/m, plain/m2)"));
	CHECK(config_profile(&c, "local/nope", &p, err, sizeof err) != 0);
	CHECK(strstr(err, "gateway \"local\" has no model \"nope\" (models: m, narrow, none, meta/llama)"));
	CHECK(config_profile(&c, "plain/narrow", &p, err, sizeof err) != 0);
	CHECK(strstr(err, "gateway \"plain\" has no model \"narrow\""));

	/* "api_key_env" holding the key itself, rather than a variable name, used
	 * to send no Authorization header at all. */
	unsetenv("KEY");
	CHECK(config_profile(&c, "local/m", &p, err, sizeof err) != 0);
	CHECK(strstr(err, "gateway \"local\": \"api_key_env\" names environment variable KEY, "
			  "which is not set"));
	setenv("KEY", "", 1);
	CHECK(config_profile(&c, "local/m", &p, err, sizeof err) != 0);
	CHECK(strstr(err, "environment variable KEY, which is empty"));
	/* Only the selected gateway's key matters. */
	CHECK(config_profile(&c, "plain/m", &p, err, sizeof err) == 0);
	setenv("KEY", "sk-unit", 1);
	CHECK(config_profile(&c, "local/m", &p, err, sizeof err) == 0);

	/* -l: config order, "* " on the one that would be used, permissions shown. */
	config_list(&c, NULL, &l);
	STREQ(l.data,
	      "* local/m           tools=read,write  mcp=searx,other  danger=yes\n"
	      "  local/narrow      tools=read  mcp=searx  danger=no\n"
	      "  local/none        tools=none  mcp=none  danger=yes\n"
	      "  local/meta/llama  tools=read,write  mcp=searx,other  danger=yes\n"
	      "  plain/m           tools=none  mcp=none  danger=no\n"
	      "  plain/m2          tools=none  mcp=none  danger=no\n");
	buf_free(&l);
	config_list(&c, "plain", &l); /* a gateway alone is its default model */
	CHECK(strstr(l.data, "* plain/m2 ") && !strstr(l.data, "* local"));
	buf_free(&l);
	config_list(&c, "gone", &l); /* nothing to mark */
	CHECK(!strstr(l.data, "*"));
	buf_free(&l);
	config_list(&c, "m", &l); /* ambiguous: nothing to mark either */
	CHECK(!strstr(l.data, "*"));
	buf_free(&l);
	unsetenv("KEY"); /* -l never needs a key */
	config_list(&c, NULL, &l);
	CHECK(strstr(l.data, "* local/m "));
	buf_free(&l);
	setenv("KEY", "sk-unit", 1);
	config_free(&c);

	/* An empty "gateways" object lists nothing at all. */
	CHECK(config_parse(&c, "{\"gateways\":{}}", 15, err, sizeof err) == 0);
	config_list(&c, NULL, &l);
	CHECK(!l.data);
	buf_free(&l);
	config_free(&c);

	/* Narrow, never widen. */
	CHECK(try_gw("\"tools\":[\"read\"]", "\"tools\":[\"read\",\"exec\"]", err, sizeof err) != 0);
	CHECK(strstr(err, "model \"g/m\": \"tools\" lists \"exec\", which gateway \"g\" does not allow"));
	CHECK(try_gw("", "\"tools\":[\"read\"]", err, sizeof err) != 0); /* a gateway grants only what it lists */
	CHECK(strstr(err, "which gateway \"g\" does not allow"));
	CHECK(try_gw("\"mcp_servers\":[\"a\"]", "\"mcp_servers\":[\"a\",\"b\"]", err, sizeof err) != 0);
	CHECK(strstr(err, "\"mcp_servers\" lists \"b\", which gateway \"g\" does not allow"));
	CHECK(try_gw("", "\"mcp_servers\":[\"a\"]", err, sizeof err) != 0);
	CHECK(try_gw("", "\"allow_danger\":true", err, sizeof err) != 0);
	CHECK(strstr(err, "\"allow_danger\" is true, but gateway \"g\" does not allow it"));
	CHECK(try_gw("\"allow_danger\":true", "\"allow_danger\":true", err, sizeof err) == 0);
	CHECK(try_gw("", "\"allow_danger\":false", err, sizeof err) == 0);
	CHECK(try_gw("\"tools\":[\"read\"]", "\"tools\":[]", err, sizeof err) == 0);

	/* Mistakes in a gateway that is not being selected are still reported. */
	CHECK(try_select("{\"gateways\":{\"a\":{\"endpoint\":\"e\",\"models\":{\"x\":{}}},"
			 "\"b\":{\"endpoint\":\"e\",\"tools\":[\"read\"],\"models\":{\"y\":{\"tools\":[\"exec\"]}}}}}",
			 "a", err, sizeof err) != 0);
	CHECK(strstr(err, "model \"b/y\""));

	/* Shape and types. */
	CHECK(try_gw("\"tools\":false", "", err, sizeof err) != 0);
	CHECK(strstr(err, "gateway \"g\": \"tools\" must be an array of \"read\", \"write\" and \"exec\""));
	CHECK(try_gw("\"tools\":[\"delete\"]", "", err, sizeof err) != 0);
	CHECK(strstr(err, "\"tools\" must be an array of"));
	CHECK(try_gw("", "\"tools\":[7]", err, sizeof err) != 0);
	CHECK(try_gw("\"mcp\":false", "", err, sizeof err) != 0);
	CHECK(strstr(err, "\"mcp\" is no longer supported: list \"mcp_servers\", and use [] for none"));
	CHECK(try_gw("", "\"mcp\":false", err, sizeof err) != 0);
	CHECK(try_gw("\"mcp_servers\":\"searx\"", "", err, sizeof err) != 0);
	CHECK(strstr(err, "\"mcp_servers\" must be an array of server names"));
	CHECK(try_gw("\"mcp_servers\":[\"ok\",\"\"]", "", err, sizeof err) != 0);
	CHECK(try_gw("\"mcp_servers\":[]", "", err, sizeof err) == 0);
	CHECK(try_gw("\"allow_danger\":\"yes\"", "", err, sizeof err) != 0);
	CHECK(strstr(err, "gateway \"g\": \"allow_danger\" must be true or false"));
	CHECK(try_gw("\"colour\":1", "", err, sizeof err) != 0);
	CHECK(strstr(err, "gateway \"g\": unknown key \"colour\""));
	CHECK(try_gw("", "\"endpoint\":\"x\"", err, sizeof err) != 0);
	CHECK(strstr(err, "model \"g/m\": unknown key \"endpoint\""));
	CHECK(try_gw("\"backend\":\"openai\"", "", err, sizeof err) != 0);
	CHECK(try_gw("\"temperature\":\"hot\"", "", err, sizeof err) != 0);
	CHECK(strstr(err, "\"temperature\" must be a number"));
	CHECK(try_gw("", "\"max_tokens\":0", err, sizeof err) != 0);
	CHECK(strstr(err, "\"max_tokens\" must be a positive integer"));
	CHECK(try_gw("\"timeout\":0", "", err, sizeof err) != 0);
	CHECK(strstr(err, "\"timeout\" must be a number of seconds"));
	CHECK(try_gw("", "\"id\":7", err, sizeof err) != 0);
	CHECK(strstr(err, "model \"g/m\": \"id\" must be a string"));
	CHECK(try_gw("", "\"system_prompt\":7", err, sizeof err) != 0);
	CHECK(try_gw("\"default_model\":\"zzz\"", "", err, sizeof err) != 0);
	CHECK(strstr(err, "gateway \"g\": \"default_model\" must name one of its models"));
	CHECK(try_gw("\"default_model\":\"m\"", "", err, sizeof err) == 0);

	CHECK(try_select("{\"gateways\":{\"g\":{\"models\":{\"m\":{}}}}}", "g", err, sizeof err) != 0);
	CHECK(strstr(err, "gateway \"g\": a gateway requires \"endpoint\""));
	CHECK(try_select("{\"gateways\":{\"g\":{\"endpoint\":\"e\"}}}", "g", err, sizeof err) != 0);
	CHECK(strstr(err, "a gateway requires a \"models\" object with a model in it"));
	CHECK(try_select("{\"gateways\":{\"g\":{\"endpoint\":\"e\",\"models\":{}}}}", "g", err, sizeof err) != 0);
	CHECK(try_select("{\"gateways\":{\"g\":{\"endpoint\":\"e\",\"models\":{\"m\":7}}}}", "g", err, sizeof err) != 0);
	CHECK(strstr(err, "model \"g/m\": must be an object"));
	CHECK(try_select("{\"gateways\":{\"g\":7}}", "g", err, sizeof err) != 0);
	CHECK(strstr(err, "gateway \"g\": must be an object"));
	CHECK(try_select("{\"gateways\":{\"a/b\":{\"endpoint\":\"e\",\"models\":{\"m\":{}}}}}", "a/b/m",
			 err, sizeof err) != 0);
	CHECK(strstr(err, "gateway name \"a/b\" must not be empty or contain \"/\""));
	CHECK(try_select("{\"gateways\":{\"\":{\"endpoint\":\"e\",\"models\":{\"m\":{}}}}}", "m",
			 err, sizeof err) != 0);
	CHECK(try_select("{\"gateways\":{\"g\":{\"endpoint\":\"e\",\"models\":{\"\":{}}}}}", "g",
			 err, sizeof err) != 0);
	CHECK(strstr(err, "a model name must not be empty"));
	CHECK(try_select("{\"gateways\":{\"g\":{\"endpoint\":\"e\",\"models\":{\"m\":{},\"m\":{}}}}}", "g",
			 err, sizeof err) != 0);
	CHECK(strstr(err, "model \"g/m\": is listed twice"));
	CHECK(try_select("{\"gateways\":{\"g\":{\"endpoint\":\"e\",\"models\":{\"m\":{}}},"
			 "\"g\":{\"endpoint\":\"e\",\"models\":{\"m\":{}}}}}", "g", err, sizeof err) != 0);
	CHECK(strstr(err, "gateway \"g\" is listed twice"));

	/* Selecting with nothing to go on. */
	CHECK(try_select("{\"gateways\":{}}", NULL, err, sizeof err) != 0);
	CHECK(strstr(err, "no model selected"));
	CHECK(try_select("{\"default\":\"gone/x\",\"gateways\":{\"g\":{\"endpoint\":\"e\",\"models\":{\"m\":{}}}}}",
			 NULL, err, sizeof err) != 0);
	CHECK(strstr(err, "unknown model or gateway \"gone/x\" (available: g/m)"));

	/* A gateway and a model with the same name are both readings of a bare name. */
	CHECK(try_select("{\"gateways\":{\"x\":{\"endpoint\":\"e\",\"default_model\":\"y\","
			 "\"models\":{\"y\":{}}},\"z\":{\"endpoint\":\"e\",\"models\":{\"x\":{}}}}}",
			 "x", err, sizeof err) != 0);
	CHECK(strstr(err, "\"x\" is ambiguous; use gateway/model: x/y, z/x"));
	/* ... unless they are the same model. */
	CHECK(try_select("{\"gateways\":{\"x\":{\"endpoint\":\"e\",\"models\":{\"x\":{}}}}}",
			 "x", err, sizeof err) == 0);

	CHECK(try_select("{\n\"gateways\":\n{,}}", "x", err, sizeof err) != 0);
	CHECK(strstr(err, "invalid JSON near line 3"));
	CHECK(try_select("[]", "x", err, sizeof err) != 0);
	CHECK(strstr(err, "must be a JSON object"));
	CHECK(try_select("{}", "x", err, sizeof err) != 0);
	CHECK(strstr(err, "\"gateways\" object"));
	CHECK(try_select("{\"timeout\":\"soon\",\"gateways\":{}}", "x", err, sizeof err) != 0);
	CHECK(strstr(err, "\"timeout\""));

	/* The old format gets a pointer to the new one, not a puzzling error. */
	CHECK(try_select("{\"profiles\":{\"x\":{\"endpoint\":\"e\",\"model\":\"m\"}}}", "x",
			 err, sizeof err) != 0);
	CHECK(strstr(err, "\"profiles\" is no longer supported"));
}

int main(void)
{
	test_buf();
	test_system();
	test_user();
	test_clean();
	test_url();
	test_mcp();
	test_tools();
	test_log();
	test_config();

	printf("unit: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
