#ifndef QQ_HTTP_H
#define QQ_HTTP_H

#include "buf.h"

#include <curl/curl.h>
#include <stddef.h>

struct cJSON;

/* One libcurl handle reused for every request (keeping the connection
 * alive), with a single deadline shared by all of them. */
struct http {
	CURL *curl;
	struct curl_slist *headers;
	long long deadline_ms;
	long timeout;
	char errbuf[CURL_ERROR_SIZE];
};

/* Always initializes c, so http_free() is safe even when this fails. */
int http_init(struct http *c, const char *api_key, long timeout, char *err, size_t errlen);

/* POST body as JSON. Returns 0 when any HTTP response arrived: *status is its
 * code, resp the raw body and *json the parsed body (NULL if not JSON, caller
 * frees). Returns -1 with a message in err on transport errors or timeout. */
int http_post_json(struct http *c, const char *url, const struct cJSON *body, long *status,
		   struct buf *resp, struct cJSON **json, char *err, size_t errlen);

/* Best error text from {"error": ...} or {"detail": ...} bodies, else the raw body. */
const char *http_error_message(const struct cJSON *json, const struct buf *resp);

void http_free(struct http *c);

#endif
