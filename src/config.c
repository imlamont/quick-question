#include "config.h"
#include "buf.h"
#include "qq.h"
#include "tools.h"

#include <cjson/cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CONFIG_MAX (1024 * 1024)

__attribute__((format(printf, 3, 4)))
static int fail(char *err, size_t errlen, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
	return -1;
}

/* Optional switch; *out keeps its value unless the member is a JSON boolean.
 * Only an explicit false turns something off, so a profile that says nothing
 * behaves as it always has. */
static int get_bool(const cJSON *obj, const char *key, int *out, const char *where,
		    char *err, size_t errlen)
{
	const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);

	if (!it || cJSON_IsNull(it))
		return 0;
	if (!cJSON_IsBool(it))
		return fail(err, errlen, "%s\"%s\" must be true or false", where, key);
	*out = cJSON_IsTrue(it);
	return 0;
}

/* Optional string member; *out is NULL when absent, null or "". */
static int get_str(const cJSON *obj, const char *key, const char **out,
		   const char *where, char *err, size_t errlen)
{
	const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);

	*out = NULL;
	if (!it || cJSON_IsNull(it))
		return 0;
	if (!cJSON_IsString(it))
		return fail(err, errlen, "%s\"%s\" must be a string", where, key);
	if (*it->valuestring)
		*out = it->valuestring;
	return 0;
}

char *config_path(void)
{
	struct buf b = {0};
	const char *v;

	if ((v = getenv("QQ_CONFIG")) && *v)
		buf_puts(&b, v);
	else if ((v = getenv("XDG_CONFIG_HOME")) && *v == '/')
		buf_appendf(&b, "%s/qq/config.json", v);
	else if ((v = getenv("HOME")) && *v)
		buf_appendf(&b, "%s/.config/qq/config.json", v);
	else
		return NULL;
	return buf_steal(&b);
}

/* What a gateway or a model may say about how it is used. Unset is NAN, 0 or
 * NULL, so a model's own value can be told from "inherit". */
struct tuning {
	const char *system_prompt;
	double temperature;
	int max_tokens;
	long timeout;
};

/* A gateway's settings, checked, for its models to inherit. */
struct gateway {
	const char *name;
	const char *endpoint;
	const char *api_key_env;
	struct tuning tune;
	int tools;                /* TOOLS_* mask it grants */
	const cJSON *mcp_servers; /* NULL when it lists none */
	int allow_danger;
	const cJSON *models;
};

static const char *const gateway_keys[] = {
	"endpoint", "api_key_env", "default_model", "system_prompt", "temperature",
	"max_tokens", "timeout", "tools", "mcp_servers", "allow_danger", "models", NULL
};
static const char *const model_keys[] = {
	"id", "system_prompt", "temperature", "max_tokens", "timeout", "tools",
	"mcp_servers", "allow_danger", NULL
};

static const struct { const char *name; int flag; } categories[] = {
	{ "read", TOOLS_READ }, { "write", TOOLS_WRITE }, { "exec", TOOLS_EXEC },
};
#define NCATEGORIES (sizeof categories / sizeof *categories)

/* Unknown keys are errors: a misspelt or retired "tools" or "mcp" that was
 * silently ignored would leave a model with more access than its config says. */
static int check_keys(const cJSON *obj, const char *const *allowed, const char *where,
		      char *err, size_t errlen)
{
	const cJSON *it;

	cJSON_ArrayForEach(it, obj) {
		int known = 0;

		for (const char *const *k = allowed; *k; k++)
			known |= !strcmp(it->string, *k);
		if (known)
			continue;
		if (!strcmp(it->string, "mcp"))
			return fail(err, errlen,
				    "%s\"mcp\" is no longer supported: list \"mcp_servers\", "
				    "and use [] for none", where);
		return fail(err, errlen, "%sunknown key \"%s\"", where, it->string);
	}
	return 0;
}

static int get_tuning(const cJSON *obj, struct tuning *t, const char *where,
		      char *err, size_t errlen)
{
	const cJSON *it;

	*t = (struct tuning){ .temperature = NAN };
	if (get_str(obj, "system_prompt", &t->system_prompt, where, err, errlen))
		return -1;
	if ((it = cJSON_GetObjectItemCaseSensitive(obj, "temperature"))) {
		if (!cJSON_IsNumber(it))
			return fail(err, errlen, "%s\"temperature\" must be a number", where);
		t->temperature = it->valuedouble;
	}
	if ((it = cJSON_GetObjectItemCaseSensitive(obj, "max_tokens"))) {
		if (!cJSON_IsNumber(it) || it->valuedouble < 1 || it->valuedouble > INT_MAX)
			return fail(err, errlen, "%s\"max_tokens\" must be a positive integer", where);
		t->max_tokens = (int)it->valuedouble;
	}
	if ((it = cJSON_GetObjectItemCaseSensitive(obj, "timeout"))) {
		if (!cJSON_IsNumber(it) || it->valuedouble < 1 || it->valuedouble > QQ_TIMEOUT_MAX)
			return fail(err, errlen, "%s\"timeout\" must be a number of seconds (1-%d)",
				    where, QQ_TIMEOUT_MAX);
		t->timeout = (long)it->valuedouble;
	}
	return 0;
}

/* "tools": an array of read, write and exec. *mask is -1 when the member is
 * absent, so a model can tell "inherit" from "none". */
static int get_tools(const cJSON *obj, int *mask, const char *where, char *err, size_t errlen)
{
	const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, "tools"), *s;

	*mask = -1;
	if (!it || cJSON_IsNull(it))
		return 0;
	if (!cJSON_IsArray(it))
		return fail(err, errlen,
			    "%s\"tools\" must be an array of \"read\", \"write\" and \"exec\"", where);
	*mask = 0;
	cJSON_ArrayForEach(s, it) {
		int known = 0;

		for (size_t i = 0; cJSON_IsString(s) && i < NCATEGORIES; i++)
			if (!strcmp(s->valuestring, categories[i].name)) {
				*mask |= categories[i].flag;
				known = 1;
			}
		if (!known)
			return fail(err, errlen,
				    "%s\"tools\" must be an array of \"read\", \"write\" and \"exec\"",
				    where);
	}
	return 0;
}

/* "mcp_servers": an array of server names, or NULL when absent. An empty array
 * is returned as such: on a model it means none, not inherit. */
static int get_servers(const cJSON *obj, const cJSON **out, const char *where,
		       char *err, size_t errlen)
{
	const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, "mcp_servers"), *s;

	*out = NULL;
	if (!it || cJSON_IsNull(it))
		return 0;
	if (!cJSON_IsArray(it))
		return fail(err, errlen, "%s\"mcp_servers\" must be an array of server names", where);
	cJSON_ArrayForEach(s, it)
		if (!cJSON_IsString(s) || !*s->valuestring)
			return fail(err, errlen,
				    "%s\"mcp_servers\" must be an array of server names", where);
	*out = it;
	return 0;
}

/* True if item is the first member of obj with its name. cJSON keeps duplicate
 * keys but only ever finds the first, so a later one would be dead config. */
static int first_of_name(const cJSON *obj, const cJSON *item)
{
	return cJSON_GetObjectItemCaseSensitive(obj, item->string) == item;
}

static int parse_gateway(const cJSON *obj, struct gateway *g, char *err, size_t errlen)
{
	char where[256];

	*g = (struct gateway){ .name = obj->string };
	snprintf(where, sizeof where, "gateway \"%s\": ", g->name);
	if (!*g->name || strchr(g->name, '/'))
		return fail(err, errlen, "gateway name \"%s\" must not be empty or contain \"/\"",
			    g->name);
	if (!cJSON_IsObject(obj))
		return fail(err, errlen, "%smust be an object", where);
	if (check_keys(obj, gateway_keys, where, err, errlen) ||
	    get_str(obj, "endpoint", &g->endpoint, where, err, errlen) ||
	    get_str(obj, "api_key_env", &g->api_key_env, where, err, errlen) ||
	    get_tuning(obj, &g->tune, where, err, errlen) ||
	    get_tools(obj, &g->tools, where, err, errlen) ||
	    get_servers(obj, &g->mcp_servers, where, err, errlen) ||
	    get_bool(obj, "allow_danger", &g->allow_danger, where, err, errlen))
		return -1;
	if (!g->endpoint)
		return fail(err, errlen, "%sa gateway requires \"endpoint\"", where);
	if (g->tools < 0)
		g->tools = 0; /* a gateway grants only what it lists */
	if (g->mcp_servers && !cJSON_GetArraySize(g->mcp_servers))
		g->mcp_servers = NULL;
	g->models = cJSON_GetObjectItemCaseSensitive(obj, "models");
	if (!cJSON_IsObject(g->models) || !g->models->child)
		return fail(err, errlen, "%sa gateway requires a \"models\" object with a model in it",
			    where);
	return 0;
}

/* One model on a checked gateway: its own keys, then what it inherits and what
 * it may only narrow. */
static int parse_model(const struct config *c, const struct gateway *g, const cJSON *m,
		       struct profile *p, char *err, size_t errlen)
{
	struct tuning t;
	const cJSON *servers, *it, *s;
	const char *id;
	char where[512];
	int mask, danger = -1;

	snprintf(where, sizeof where, "model \"%s/%s\": ", g->name, m->string);
	if (!*m->string)
		return fail(err, errlen, "gateway \"%s\": a model name must not be empty", g->name);
	if (!cJSON_IsObject(m))
		return fail(err, errlen, "%smust be an object", where);
	if (!first_of_name(g->models, m))
		return fail(err, errlen, "%sis listed twice", where);
	if (check_keys(m, model_keys, where, err, errlen) ||
	    get_str(m, "id", &id, where, err, errlen) ||
	    get_tuning(m, &t, where, err, errlen) ||
	    get_tools(m, &mask, where, err, errlen) ||
	    get_servers(m, &servers, where, err, errlen) ||
	    get_bool(m, "allow_danger", &danger, where, err, errlen))
		return -1;

	/* A model may narrow what its gateway grants, never widen it. */
	for (size_t i = 0; mask >= 0 && i < NCATEGORIES; i++)
		if ((mask & categories[i].flag) && !(g->tools & categories[i].flag))
			return fail(err, errlen,
				    "%s\"tools\" lists \"%s\", which gateway \"%s\" does not allow",
				    where, categories[i].name, g->name);
	if (servers)
		cJSON_ArrayForEach(it, servers) {
			int granted = 0;

			if (g->mcp_servers)
				cJSON_ArrayForEach(s, g->mcp_servers)
					granted |= !strcmp(s->valuestring, it->valuestring);
			if (!granted)
				return fail(err, errlen,
					    "%s\"mcp_servers\" lists \"%s\", which gateway \"%s\" does not allow",
					    where, it->valuestring, g->name);
		}
	if (danger > 0 && !g->allow_danger)
		return fail(err, errlen, "%s\"allow_danger\" is true, but gateway \"%s\" does not allow it",
			    where, g->name);

	*p = (struct profile){
		.gateway = g->name,
		.name = m->string,
		.model = id ? id : m->string,
		.endpoint = g->endpoint,
		.api_key_env = g->api_key_env,
		.gateway_prompt = g->tune.system_prompt,
		.system_prompt = t.system_prompt,
		.temperature = !isnan(t.temperature) ? t.temperature : g->tune.temperature,
		.max_tokens = t.max_tokens ? t.max_tokens : g->tune.max_tokens,
		.timeout = t.timeout ? t.timeout : g->tune.timeout ? g->tune.timeout : c->timeout,
		.tools = mask >= 0 ? mask : g->tools,
		.allow_danger = danger >= 0 ? danger : g->allow_danger,
		.mcp_servers = servers ? servers : g->mcp_servers,
	};
	if (p->mcp_servers && !cJSON_GetArraySize(p->mcp_servers))
		p->mcp_servers = NULL;
	return 0;
}

/* The model a gateway uses when only the gateway is named. Only for a gateway
 * that has been checked, so "default_model" is a string naming a real model. */
static const cJSON *default_model(const cJSON *gateway)
{
	const cJSON *models = cJSON_GetObjectItemCaseSensitive(gateway, "models");
	const cJSON *name = cJSON_GetObjectItemCaseSensitive(gateway, "default_model");

	if (cJSON_IsString(name) && *name->valuestring)
		return cJSON_GetObjectItemCaseSensitive(models, name->valuestring);
	return models->child;
}

/* Everything that can be wrong with the file itself, so a mistake in a gateway
 * that isn't being used is still reported. API keys are left for the selected
 * model: not every gateway's key has to be exported at once. */
static int validate(const struct config *c, char *err, size_t errlen)
{
	const cJSON *gateways = cJSON_GetObjectItemCaseSensitive(c->root, "gateways"), *g, *m;
	struct gateway gw;
	struct profile p;

	cJSON_ArrayForEach(g, gateways) {
		const cJSON *dm = cJSON_GetObjectItemCaseSensitive(g, "default_model");

		if (!first_of_name(gateways, g))
			return fail(err, errlen, "gateway \"%s\" is listed twice", g->string);
		if (parse_gateway(g, &gw, err, errlen))
			return -1;
		if (dm && !cJSON_IsNull(dm) &&
		    (!cJSON_IsString(dm) || !cJSON_GetObjectItemCaseSensitive(gw.models, dm->valuestring)))
			return fail(err, errlen,
				    "gateway \"%s\": \"default_model\" must name one of its models",
				    g->string);
		cJSON_ArrayForEach(m, gw.models)
			if (parse_model(c, &gw, m, &p, err, errlen))
				return -1;
	}
	return 0;
}

int config_parse(struct config *c, const char *json, size_t len, char *err, size_t errlen)
{
	const cJSON *it;

	c->root = cJSON_ParseWithLength(json, len);
	if (!c->root) {
		const char *at = cJSON_GetErrorPtr();
		int line = 1;

		if (at && at >= json && at <= json + len)
			for (const char *p = json; p < at; p++)
				line += *p == '\n';
		return fail(err, errlen, "invalid JSON near line %d", line);
	}
	if (!cJSON_IsObject(c->root))
		return fail(err, errlen, "config must be a JSON object");
	if (get_str(c->root, "default", &c->default_sel, "", err, errlen) ||
	    get_str(c->root, "system_prompt", &c->system_prompt, "", err, errlen))
		return -1;

	c->timeout = QQ_DEFAULT_TIMEOUT;
	it = cJSON_GetObjectItemCaseSensitive(c->root, "timeout");
	if (it) {
		if (!cJSON_IsNumber(it) || it->valuedouble < 1 || it->valuedouble > QQ_TIMEOUT_MAX)
			return fail(err, errlen, "\"timeout\" must be a number of seconds (1-%d)",
				    QQ_TIMEOUT_MAX);
		c->timeout = (long)it->valuedouble;
	}

	if (cJSON_GetObjectItemCaseSensitive(c->root, "profiles"))
		return fail(err, errlen,
			    "\"profiles\" is no longer supported: put each endpoint in a "
			    "\"gateways\" entry and list its models under \"models\" "
			    "(see config.example.json)");
	if (!cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(c->root, "gateways")))
		return fail(err, errlen, "config needs a \"gateways\" object");
	return validate(c, err, errlen);
}

int config_load(struct config *c, const char *path, char *err, size_t errlen)
{
	struct buf b = {0};
	char msg[512];
	int fd, r, e;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		if (errno == ENOENT)
			return fail(err, errlen, "config not found: %s (start from config.example.json)",
				    path);
		return fail(err, errlen, "cannot open %s: %s", path, strerror(errno));
	}
	r = buf_read_fd(&b, fd, CONFIG_MAX);
	e = errno;
	close(fd);
	if (r) {
		buf_free(&b);
		if (r == -2)
			return fail(err, errlen, "%s: file too large", path);
		return fail(err, errlen, "cannot read %s: %s", path, strerror(e));
	}

	r = config_parse(c, b.data, b.len, msg, sizeof msg);
	buf_free(&b);
	if (r)
		return fail(err, errlen, "%s: %s", path, msg);
	return 0;
}

/* "gateway/model" for every model, in config order. */
static void qualified_names(const struct config *c, struct buf *out)
{
	const cJSON *g, *m;

	cJSON_ArrayForEach(g, cJSON_GetObjectItemCaseSensitive(c->root, "gateways"))
		cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(g, "models"))
			buf_appendf(out, "%s%s/%s", out->len ? ", " : "", g->string, m->string);
}

/* Find the gateway and model a selection names. The config has been validated,
 * so every gateway has models. */
static int select_model(const struct config *c, const char *sel, const cJSON **gw,
			const cJSON **model, char *err, size_t errlen)
{
	const cJSON *gateways = cJSON_GetObjectItemCaseSensitive(c->root, "gateways"), *g, *m;
	const char *slash = strchr(sel, '/');
	struct buf names = {0};
	int found = 0;

	/* "gateway/model": the "/" only counts after a real gateway name, so a model
	 * id like "meta/llama-3" is left alone unless a gateway is called "meta". */
	if (slash)
		cJSON_ArrayForEach(g, gateways) {
			size_t n = strlen(g->string);

			if ((size_t)(slash - sel) != n || strncmp(sel, g->string, n))
				continue;
			m = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(g, "models"),
							     slash + 1);
			if (!m) {
				cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(g, "models"))
					buf_appendf(&names, "%s%s", names.len ? ", " : "", m->string);
				fail(err, errlen, "gateway \"%s\" has no model \"%s\" (models: %s)",
				     g->string, slash + 1, names.data);
				buf_free(&names);
				return -1;
			}
			*gw = g;
			*model = m;
			return 0;
		}

	/* Otherwise a bare model name, or a gateway name standing for its default
	 * model. Every way of reading it counts, so a clash is reported, not guessed. */
	cJSON_ArrayForEach(g, gateways) {
		const cJSON *hit[2];

		hit[0] = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(g, "models"), sel);
		hit[1] = !strcmp(g->string, sel) ? default_model(g) : NULL;
		if (hit[1] == hit[0]) /* the gateway's default is the model of that name */
			hit[1] = NULL;
		for (int i = 0; i < 2; i++) {
			if (!hit[i])
				continue;
			if (!found++) {
				*gw = g;
				*model = hit[i];
			}
			buf_appendf(&names, "%s%s/%s", names.len ? ", " : "", g->string, hit[i]->string);
		}
	}
	if (found == 1) {
		buf_free(&names);
		return 0;
	}
	if (found > 1) {
		fail(err, errlen, "\"%s\" is ambiguous; use gateway/model: %s", sel, names.data);
		buf_free(&names);
		return -1;
	}
	qualified_names(c, &names);
	fail(err, errlen, "unknown model or gateway \"%s\" (available: %s)", sel,
	     names.data ? names.data : "none");
	buf_free(&names);
	return -1;
}

int config_profile(const struct config *c, const char *sel, struct profile *p,
		   char *err, size_t errlen)
{
	const cJSON *gw, *m;
	struct gateway g;

	if (!sel)
		sel = c->default_sel;
	if (!sel)
		return fail(err, errlen, "no model selected: use -p, or set a default with -d");
	if (select_model(c, sel, &gw, &m, err, errlen) ||
	    parse_gateway(gw, &g, err, errlen) || parse_model(c, &g, m, p, err, errlen))
		return -1;

	/* "api_key_env" is a variable name, not the key. Catch the mix-up here:
	 * otherwise no Authorization header goes out and the endpoint's reply to
	 * an unauthenticated request is the only clue. */
	if (p->api_key_env) {
		const char *key = getenv(p->api_key_env);

		if (!key || !*key)
			return fail(err, errlen,
				    "gateway \"%s\": \"api_key_env\" names environment variable %s, which is %s",
				    p->gateway, p->api_key_env, key ? "empty" : "not set");
	}
	return 0;
}

static int write_all(int fd, const char *s, size_t n)
{
	while (n) {
		ssize_t w = write(fd, s, n);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		s += w;
		n -= (size_t)w;
	}
	return 0;
}

static void join_tools(int mask, struct buf *out)
{
	for (size_t i = 0; i < NCATEGORIES; i++)
		if (mask & categories[i].flag)
			buf_appendf(out, "%s%s", out->len ? "," : "", categories[i].name);
	if (!out->len)
		buf_puts(out, "none");
}

static void join_servers(const cJSON *list, struct buf *out)
{
	const cJSON *s;

	cJSON_ArrayForEach(s, list)
		buf_appendf(out, "%s%s", out->len ? "," : "", s->valuestring);
	if (!out->len)
		buf_puts(out, "none");
}

void config_list(const struct config *c, const char *sel, struct buf *out)
{
	const cJSON *gateways = cJSON_GetObjectItemCaseSensitive(c->root, "gateways"), *g, *m;
	const cJSON *active_g = NULL, *active_m = NULL;
	char scratch[256];
	size_t width = 0;

	if (!sel)
		sel = c->default_sel;
	if (sel && select_model(c, sel, &active_g, &active_m, scratch, sizeof scratch))
		active_g = active_m = NULL;

	cJSON_ArrayForEach(g, gateways)
		cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(g, "models")) {
			size_t n = strlen(g->string) + 1 + strlen(m->string);

			width = n > width ? n : width;
		}

	cJSON_ArrayForEach(g, gateways) {
		struct gateway gw;

		if (parse_gateway(g, &gw, scratch, sizeof scratch))
			continue;
		cJSON_ArrayForEach(m, gw.models) {
			struct profile p;
			struct buf name = {0}, tools = {0}, mcp = {0};

			if (parse_model(c, &gw, m, &p, scratch, sizeof scratch))
				continue;
			buf_appendf(&name, "%s/%s", g->string, m->string);
			join_tools(p.tools, &tools);
			join_servers(p.mcp_servers, &mcp);
			buf_appendf(out, "%s%-*s  tools=%s  mcp=%s  danger=%s\n",
				    g == active_g && m == active_m ? "* " : "  ", (int)width,
				    name.data, tools.data, mcp.data, p.allow_danger ? "yes" : "no");
			buf_free(&name);
			buf_free(&tools);
			buf_free(&mcp);
		}
	}
}

int config_set_default(struct config *c, const char *path, const char *name,
		       char *err, size_t errlen)
{
	struct profile p;
	struct buf tmp = {0}, qual = {0};
	struct stat st;
	cJSON *item;
	char *json = NULL, *real = NULL, *dir = NULL;
	int fd = -1, created = 0, stored, rc = -2;

	if (config_profile(c, name, &p, err, errlen))
		return -1;

	buf_appendf(&qual, "%s/%s", p.gateway, p.name);
	if (!(item = cJSON_CreateString(qual.data))) {
		fail(err, errlen, "out of memory");
		goto out;
	}
	/* Replace in place so "default" keeps its position in the file. */
	if (cJSON_GetObjectItemCaseSensitive(c->root, "default"))
		stored = cJSON_ReplaceItemInObjectCaseSensitive(c->root, "default", item);
	else
		stored = cJSON_AddItemToObject(c->root, "default", item);
	if (!stored) {
		cJSON_Delete(item);
		fail(err, errlen, "out of memory");
		goto out;
	}
	c->default_sel = item->valuestring;

	/* Write next to the real file (following symlinks), then rename over it. */
	if (!(real = realpath(path, NULL)) || stat(real, &st)) {
		fail(err, errlen, "cannot resolve %s: %s", path, strerror(errno));
		goto out;
	}
	if (!(json = cJSON_Print(c->root)) || !(dir = strdup(real))) {
		fail(err, errlen, "out of memory");
		goto out;
	}
	buf_appendf(&tmp, "%s/.qq-config.XXXXXX", dirname(dir));
	if ((fd = mkstemp(tmp.data)) < 0) {
		fail(err, errlen, "cannot create %s: %s", tmp.data, strerror(errno));
		goto out;
	}
	created = 1;
	if (fchmod(fd, st.st_mode & 07777) || write_all(fd, json, strlen(json)) ||
	    write_all(fd, "\n", 1) || fsync(fd)) {
		fail(err, errlen, "cannot write %s: %s", tmp.data, strerror(errno));
		goto out;
	}
	if (close(fd)) {
		fd = -1;
		fail(err, errlen, "cannot write %s: %s", tmp.data, strerror(errno));
		goto out;
	}
	fd = -1;
	if (rename(tmp.data, real)) {
		fail(err, errlen, "cannot replace %s: %s", real, strerror(errno));
		goto out;
	}
	rc = 0;
out:
	if (fd >= 0)
		close(fd);
	if (rc && created)
		unlink(tmp.data);
	buf_free(&tmp);
	buf_free(&qual);
	cJSON_free(json);
	free(real);
	free(dir);
	return rc;
}

void config_free(struct config *c)
{
	cJSON_Delete(c->root);
	*c = (struct config){0};
}
