#include "config.h"
#include "buf.h"
#include "qq.h"

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
	if (get_str(c->root, "default", &c->default_profile, "", err, errlen) ||
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

	if (!cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(c->root, "profiles")))
		return fail(err, errlen, "config needs a \"profiles\" object");
	return 0;
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

int config_profile(const struct config *c, const char *name, struct profile *p,
		   char *err, size_t errlen)
{
	const cJSON *profiles = cJSON_GetObjectItemCaseSensitive(c->root, "profiles");
	const cJSON *obj, *it;
	const char *backend;
	char where[128];

	if (!name)
		name = c->default_profile;
	if (!name)
		return fail(err, errlen, "no profile selected: use -p, or set a default with -d");

	obj = cJSON_GetObjectItemCaseSensitive(profiles, name);
	if (!cJSON_IsObject(obj)) {
		struct buf names = {0};

		cJSON_ArrayForEach(it, profiles)
			if (cJSON_IsObject(it))
				buf_appendf(&names, "%s%s", names.len ? ", " : "", it->string);
		fail(err, errlen, "unknown profile \"%s\" (available: %s)", name,
		     names.data ? names.data : "none");
		buf_free(&names);
		return -1;
	}

	*p = (struct profile){ .name = obj->string, .temperature = NAN };
	snprintf(where, sizeof where, "profile \"%s\": ", name);
	if (get_str(obj, "backend", &backend, where, err, errlen) ||
	    get_str(obj, "endpoint", &p->endpoint, where, err, errlen) ||
	    get_str(obj, "model", &p->model, where, err, errlen) ||
	    get_str(obj, "api_key_env", &p->api_key_env, where, err, errlen) ||
	    get_str(obj, "system_prompt", &p->system_prompt, where, err, errlen))
		return -1;

	/* "backend" is optional: OpenAI-compatible endpoints are the only kind. */
	if (backend && strcmp(backend, "openai"))
		return fail(err, errlen, "%sunknown backend \"%s\" (only openai is supported)",
			    where, backend);
	if (!p->endpoint || !p->model)
		return fail(err, errlen, "%sa profile requires \"endpoint\" and \"model\"", where);

	if ((it = cJSON_GetObjectItemCaseSensitive(obj, "temperature"))) {
		if (!cJSON_IsNumber(it))
			return fail(err, errlen, "%s\"temperature\" must be a number", where);
		p->temperature = it->valuedouble;
	}
	if ((it = cJSON_GetObjectItemCaseSensitive(obj, "max_tokens"))) {
		if (!cJSON_IsNumber(it) || it->valuedouble < 1 || it->valuedouble > INT_MAX)
			return fail(err, errlen, "%s\"max_tokens\" must be a positive integer", where);
		p->max_tokens = (int)it->valuedouble;
	}
	if ((it = cJSON_GetObjectItemCaseSensitive(obj, "mcp_servers"))) {
		const cJSON *s;

		if (!cJSON_IsArray(it))
			return fail(err, errlen, "%s\"mcp_servers\" must be an array of server names",
				    where);
		cJSON_ArrayForEach(s, it)
			if (!cJSON_IsString(s) || !*s->valuestring)
				return fail(err, errlen,
					    "%s\"mcp_servers\" must be an array of server names", where);
		if (cJSON_GetArraySize(it) > 0)
			p->mcp_servers = it;
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

int config_set_default(struct config *c, const char *path, const char *name,
		       char *err, size_t errlen)
{
	struct profile p;
	struct buf tmp = {0};
	struct stat st;
	cJSON *item;
	char *json = NULL, *real = NULL, *dir = NULL;
	int fd = -1, created = 0, stored, rc = -2;

	if (config_profile(c, name, &p, err, errlen))
		return -1;

	if (!(item = cJSON_CreateString(name))) {
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
	c->default_profile = item->valuestring;

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
