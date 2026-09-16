#ifndef QQ_CONFIG_H
#define QQ_CONFIG_H

#include <stddef.h>

struct cJSON;
struct buf;

/* An OpenAI-compatible endpoint. All strings point into the config's cJSON
 * tree (or argv for overrides); optional ones are NULL when absent or empty. */
struct profile {
	const char *name;
	const char *endpoint;      /* required */
	const char *model;         /* required */
	const char *api_key_env;
	const char *system_prompt;
	double temperature;        /* NAN when unset */
	int max_tokens;            /* 0 when unset */
	int tools;                 /* "tools": false forbids -r, -w and -x */
	int mcp;                   /* "mcp": false ignores "mcp_servers" */
	int allow_danger;          /* "allow_danger": true is what -y requires */
	const struct cJSON *mcp_servers; /* non-empty array of names, or NULL */
};

struct config {
	struct cJSON *root;
	const char *default_profile;
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

/* Resolve a profile by name, or the default when name is NULL. */
int config_profile(const struct config *c, const char *name, struct profile *p,
		   char *err, size_t errlen);

/* Append one profile name per line to out, in config order, marking the one
 * that would be used with "* ". That is active, or the default when active is
 * NULL. Profiles are not validated, so a broken one still gets listed. */
void config_list(const struct config *c, const char *active, struct buf *out);

/* Set "default" to name and atomically rewrite the file at path.
 * Returns -1 for an invalid profile, -2 for I/O errors. */
int config_set_default(struct config *c, const char *path, const char *name,
		       char *err, size_t errlen);

void config_free(struct config *c);

#endif
