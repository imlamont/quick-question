#ifndef QQ_CONFIG_H
#define QQ_CONFIG_H

#include <stddef.h>

struct cJSON;
struct buf;

/* One model on one gateway, with everything inherited already applied. All
 * strings point into the config's cJSON tree; optional ones are NULL when
 * absent or empty. */
struct profile {
	const char *gateway;
	const char *name;          /* the model's name in the config, as typed after "gateway/" */
	const char *model;         /* the id sent to the API ("id", else name) */
	const char *endpoint;
	const char *api_key_env;
	const char *gateway_prompt; /* gateway's "system_prompt" */
	const char *system_prompt;  /* model's "system_prompt" */
	double temperature;        /* NAN when unset */
	int max_tokens;            /* 0 when unset */
	long timeout;              /* model, else gateway, else top level, in seconds */
	int tools;                 /* TOOLS_* mask that -r, -w and -x are allowed to use */
	int allow_danger;          /* what -y requires */
	const struct cJSON *mcp_servers; /* non-empty array of names, or NULL */
};

struct config {
	struct cJSON *root;
	const char *default_sel;   /* "default": "gateway/model" */
	const char *system_prompt;
	long timeout;
};

/* Config file location: $QQ_CONFIG, $XDG_CONFIG_HOME/qq/config.json, then
 * $HOME/.config/qq/config.json. Returns malloc'd path or NULL. */
char *config_path(void);

/* All return 0 on success or nonzero with a message in err.
 * Call config_free() afterwards regardless of the result. */
int config_load(struct config *c, const char *path, char *err, size_t errlen);
int config_parse(struct config *c, const char *json, size_t len, char *err, size_t errlen);

/* Resolve a selection to a model on a gateway, or the default when sel is
 * NULL. sel is "gateway/model", a model name found on exactly one gateway, or a
 * gateway name for its default model. A "/" splits only after a gateway name,
 * so model ids like "meta/llama-3" still work bare. */
int config_profile(const struct config *c, const char *sel, struct profile *p,
		   char *err, size_t errlen);

/* Append one line per model, in config order, as "gateway/model" followed by
 * its effective tools, mcp servers and allow_danger. The one that would be used
 * (sel, or the default when sel is NULL) starts with "* ". API keys are not
 * checked, so a model whose key is missing is still listed. */
void config_list(const struct config *c, const char *sel, struct buf *out);

/* Set "default" to the qualified "gateway/model" that sel resolves to and
 * atomically rewrite the file at path. Returns -1 for an invalid selection,
 * -2 for I/O errors. */
int config_set_default(struct config *c, const char *path, const char *name,
		       char *err, size_t errlen);

void config_free(struct config *c);

#endif
