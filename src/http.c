#include "http.h"
#include "log.h"
#include "proc.h"
#include "qq.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESPONSE_MAX (16 * 1024 * 1024)

static size_t on_write(char *data, size_t size, size_t nmemb, void *userdata)
{
	struct buf *b = userdata;
	size_t n = size * nmemb;

	if (b->len + n > RESPONSE_MAX)
		return 0; /* aborts the transfer */
	buf_append(b, data, n);
	return n;
}

int http_init(struct http *c, const char *api_key, long timeout, char *err, size_t errlen)
{
	struct curl_slist *list;

	*c = (struct http){ .timeout = timeout, .deadline_ms = proc_now_ms() + timeout * 1000LL };
	c->curl = curl_easy_init();
	c->headers = curl_slist_append(NULL, "Content-Type: application/json");
	if (!c->curl || !c->headers) {
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	if (api_key && *api_key) {
		struct buf auth = {0};

		buf_appendf(&auth, "Authorization: Bearer %s", api_key);
		list = curl_slist_append(c->headers, auth.data);
		buf_free(&auth);
		if (!list) {
			snprintf(err, errlen, "out of memory");
			return -1;
		}
		c->headers = list;
	}

	curl_easy_setopt(c->curl, CURLOPT_HTTPHEADER, c->headers);
	curl_easy_setopt(c->curl, CURLOPT_WRITEFUNCTION, on_write);
	curl_easy_setopt(c->curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(c->curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c->curl, CURLOPT_ERRORBUFFER, c->errbuf);
	curl_easy_setopt(c->curl, CURLOPT_USERAGENT, "qq/" QQ_VERSION);
	return 0;
}

int http_post_json(struct http *c, const char *url, const cJSON *body, long *status,
		   struct buf *resp, cJSON **json, char *err, size_t errlen)
{
	long long left = c->deadline_ms - proc_now_ms(), sent_ms;
	CURLcode rc;
	char *data;

	*status = 0;
	*json = NULL;
	resp->len = 0;
	if (resp->data)
		resp->data[0] = '\0';

	if (left <= 0) {
		snprintf(err, errlen, "timed out after %lds", c->timeout);
		return -1;
	}
	if (!(data = cJSON_PrintUnformatted(body))) {
		snprintf(err, errlen, "out of memory");
		return -1;
	}

	curl_easy_setopt(c->curl, CURLOPT_URL, url);
	curl_easy_setopt(c->curl, CURLOPT_POSTFIELDS, data);
	curl_easy_setopt(c->curl, CURLOPT_POSTFIELDSIZE, (long)strlen(data));
	curl_easy_setopt(c->curl, CURLOPT_WRITEDATA, resp);
	curl_easy_setopt(c->curl, CURLOPT_TIMEOUT_MS, (long)left);
	c->errbuf[0] = '\0';
	/* The body only; the Authorization header stays out of the log. */
	log_printf("request %s %s", url, data);
	sent_ms = proc_now_ms();
	rc = curl_easy_perform(c->curl);
	cJSON_free(data);
	if (rc != CURLE_OK) {
		snprintf(err, errlen, "%s: %s", url, *c->errbuf ? c->errbuf : curl_easy_strerror(rc));
		log_printf("request-failed %s", err);
		return -1;
	}

	curl_easy_getinfo(c->curl, CURLINFO_RESPONSE_CODE, status);
	if (log_on()) {
		char *shown = log_escape(resp->data, resp->len);

		log_printf("response %ld %lldms %s", *status, proc_now_ms() - sent_ms, shown);
		free(shown);
	}
	*json = cJSON_ParseWithLength(resp->data ? resp->data : "", resp->len);
	return 0;
}

const char *http_error_message(const cJSON *json, const struct buf *resp)
{
	static const char *const keys[] = { "error", "detail" };

	/* {"error": "..."}, {"error": {"message": "..."}}, and the same under "detail" */
	for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
		const cJSON *e = cJSON_GetObjectItemCaseSensitive(json, keys[i]);

		if (!cJSON_IsString(e))
			e = cJSON_GetObjectItemCaseSensitive(e, "message");
		if (cJSON_IsString(e))
			return e->valuestring;
	}
	return resp->data && *resp->data ? resp->data : "(empty body)";
}

void http_free(struct http *c)
{
	curl_slist_free_all(c->headers);
	curl_easy_cleanup(c->curl);
	c->headers = NULL;
	c->curl = NULL;
}
