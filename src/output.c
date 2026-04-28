#define _POSIX_C_SOURCE 200809L

#include "syswatch.h"

#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef enum {
	OUTPUT_STDOUT = 0,
	OUTPUT_FILE,
	OUTPUT_HTTP_POST
} output_mode_t;

typedef struct {
	output_mode_t mode;
	FILE *file_handle;
	char output_path[256];

	char output_url[256];
	int batch_size;
	int batch_interval_seconds;
	int retry_max_attempts;
	int retry_backoff_seconds;

	char **batch_items;
	size_t batch_count;
	size_t batch_capacity;
	struct timespec batch_started_at;
	bool batch_started;
	bool curl_ready;
} output_state_t;

static output_state_t g_output;

static char *xstrdup(const char *text)
{
	size_t len;
	char *copy;

	if (!text) {
		return NULL;
	}

	len = strlen(text) + 1;
	copy = malloc(len);
	if (!copy) {
		return NULL;
	}

	memcpy(copy, text, len);
	return copy;
}

void json_escape_string(const char *input, char *output, size_t output_size)
{
	size_t written;
	const unsigned char *cursor;

	if (!output || output_size == 0) {
		return;
	}

	output[0] = '\0';
	if (!input) {
		return;
	}

	written = 0;
	for (cursor = (const unsigned char *)input; *cursor != '\0' && written + 1 < output_size; cursor++) {
		const char *replacement = NULL;
		char unicode_escape[7];

		switch (*cursor) {
		case '"': replacement = "\\\""; break;
		case '\\': replacement = "\\\\"; break;
		case '\b': replacement = "\\b"; break;
		case '\f': replacement = "\\f"; break;
		case '\n': replacement = "\\n"; break;
		case '\r': replacement = "\\r"; break;
		case '\t': replacement = "\\t"; break;
		default:
			if (*cursor < 0x20) {
				snprintf(unicode_escape, sizeof(unicode_escape), "\\u%04x", *cursor);
				replacement = unicode_escape;
			}
			break;
		}

		if (replacement) {
			size_t repl_len = strlen(replacement);
			if (written + repl_len >= output_size) {
				break;
			}
			memcpy(output + written, replacement, repl_len);
			written += repl_len;
			continue;
		}

		output[written++] = (char)*cursor;
	}

	output[written] = '\0';
}

static void free_batch_items(void)
{
	size_t i;

	for (i = 0; i < g_output.batch_count; i++) {
		free(g_output.batch_items[i]);
		g_output.batch_items[i] = NULL;
	}
	g_output.batch_count = 0;
	g_output.batch_started = false;
}

static void batch_reset(void)
{
	free_batch_items();
	if (g_output.batch_capacity == 0) {
		g_output.batch_capacity = 16;
		g_output.batch_items = calloc(g_output.batch_capacity, sizeof(char *));
	}
}

static int batch_ensure_capacity(size_t needed)
{
	char **new_items;
	size_t new_capacity;

	if (needed <= g_output.batch_capacity) {
		return 0;
	}

	new_capacity = g_output.batch_capacity == 0 ? 16 : g_output.batch_capacity;
	while (new_capacity < needed) {
		new_capacity *= 2;
	}

	new_items = realloc(g_output.batch_items, new_capacity * sizeof(char *));
	if (!new_items) {
		return -1;
	}

	memset(new_items + g_output.batch_capacity, 0, (new_capacity - g_output.batch_capacity) * sizeof(char *));
	g_output.batch_items = new_items;
	g_output.batch_capacity = new_capacity;
	return 0;
}

static double monotonic_seconds(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return 0.0;
	}

	return (double)now.tv_sec + ((double)now.tv_nsec / 1000000000.0);
}

static double batch_age_seconds(void)
{
	double started;
	double now;

	if (!g_output.batch_started) {
		return 0.0;
	}

	started = (double)g_output.batch_started_at.tv_sec + ((double)g_output.batch_started_at.tv_nsec / 1000000000.0);
	now = monotonic_seconds();
	return now - started;
}

static void batch_start_if_needed(void)
{
	if (g_output.batch_started) {
		return;
	}
	if (clock_gettime(CLOCK_MONOTONIC, &g_output.batch_started_at) == 0) {
		g_output.batch_started = true;
	}
}

static int http_post_batch(void)
{
	CURL *curl;
	char *body;
	size_t body_len;
	size_t i;
	int attempt;
	long response_code;
	CURLcode code;

	if (g_output.batch_count == 0) {
		return 0;
	}

	body_len = 2;
	for (i = 0; i < g_output.batch_count; i++) {
		body_len += strlen(g_output.batch_items[i]) + 1;
	}

	body = malloc(body_len + 1);
	if (!body) {
		return -1;
	}

	body[0] = '[';
	body[1] = '\0';
	for (i = 0; i < g_output.batch_count; i++) {
		strcat(body, g_output.batch_items[i]);
		if (i + 1 < g_output.batch_count) {
			strcat(body, ",");
		}
	}
	strcat(body, "]");

	curl = curl_easy_init();
	if (!curl) {
		free(body);
		return -1;
	}

	for (attempt = 0; attempt <= g_output.retry_max_attempts; attempt++) {
		struct curl_slist *headers = NULL;

		headers = curl_slist_append(headers, "Content-Type: application/json");
		curl_easy_setopt(curl, CURLOPT_URL, g_output.output_url);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

		code = curl_easy_perform(curl);
		response_code = 0;
		if (code == CURLE_OK) {
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
			if (response_code >= 200 && response_code < 300) {
				curl_slist_free_all(headers);
				curl_easy_cleanup(curl);
				free(body);
				free_batch_items();
				return 0;
			}
			if (response_code >= 400 && response_code < 500 && response_code != 429) {
				fprintf(stderr, "syswatch: dropping HTTP batch due to client error %ld\n", response_code);
				curl_slist_free_all(headers);
				curl_easy_cleanup(curl);
				free(body);
				free_batch_items();
				return 0;
			}
		}

		curl_slist_free_all(headers);
		if (attempt < g_output.retry_max_attempts) {
			unsigned int backoff_seconds;
			unsigned int cap_seconds = 60;

			backoff_seconds = (unsigned int)g_output.retry_backoff_seconds;
			if (backoff_seconds == 0) {
				backoff_seconds = 1;
			}
			backoff_seconds <<= attempt;
			if (backoff_seconds > cap_seconds) {
				backoff_seconds = cap_seconds;
			}
			sleep(backoff_seconds);
		}
	}

	fprintf(stderr, "syswatch: dropping HTTP batch after %d retries\n", g_output.retry_max_attempts);
	curl_easy_cleanup(curl);
	free(body);
	free_batch_items();
	return -1;
}

static int maybe_flush_http_batch(void)
{
	if (g_output.batch_count == 0) {
		return 0;
	}

	if ((int)g_output.batch_count >= g_output.batch_size) {
		return http_post_batch();
	}

	if (g_output.batch_started && g_output.batch_interval_seconds > 0 && batch_age_seconds() >= (double)g_output.batch_interval_seconds) {
		return http_post_batch();
	}

	return 0;
}

int output_init(const syswatch_config_t *cfg, char **err)
{
	if (!cfg) {
		if (err) {
			*err = xstrdup("output error: null config");
		}
		return -1;
	}

	memset(&g_output, 0, sizeof(g_output));
	g_output.mode = OUTPUT_STDOUT;
	g_output.batch_size = cfg->output_batch_size > 0 ? cfg->output_batch_size : 50;
	g_output.batch_interval_seconds = cfg->output_batch_interval_seconds > 0 ? cfg->output_batch_interval_seconds : 2;
	g_output.retry_max_attempts = cfg->output_retry_max_attempts >= 0 ? cfg->output_retry_max_attempts : 5;
	g_output.retry_backoff_seconds = cfg->output_retry_backoff_seconds >= 0 ? cfg->output_retry_backoff_seconds : 1;

	if (cfg->output_type[0] == '\0' || strcmp(cfg->output_type, "stdout") == 0) {
		g_output.mode = OUTPUT_STDOUT;
		fprintf(stderr, "syswatch: output_init selected stdout\n");
		return 0;
	}

	if (strcmp(cfg->output_type, "file") == 0) {
		if (cfg->output_path[0] == '\0') {
			if (err) {
				*err = xstrdup("output error: output.path required for file output");
			}
			return -1;
		}

		g_output.file_handle = fopen(cfg->output_path, "a");
		if (!g_output.file_handle) {
			if (err) {
				char message[512];
				snprintf(message, sizeof(message), "output error: failed to open '%s'", cfg->output_path);
				*err = xstrdup(message);
			}
			return -1;
		}

		g_output.mode = OUTPUT_FILE;
		snprintf(g_output.output_path, sizeof(g_output.output_path), "%s", cfg->output_path);
		fprintf(stderr, "syswatch: output_init selected file -> %s\n", g_output.output_path);
		return 0;
	}

	if (strcmp(cfg->output_type, "http_post") == 0) {
		if (cfg->output_url[0] == '\0') {
			if (err) {
				*err = xstrdup("output error: output.url required for http_post");
			}
			return -1;
		}

		if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
			if (err) {
				*err = xstrdup("output error: failed to initialize libcurl");
			}
			return -1;
		}

		g_output.mode = OUTPUT_HTTP_POST;
		g_output.curl_ready = true;
		snprintf(g_output.output_url, sizeof(g_output.output_url), "%s", cfg->output_url);
		fprintf(stderr, "syswatch: output_init selected http_post -> %s\n", g_output.output_url);
		batch_reset();
		if (!g_output.batch_items) {
			if (err) {
				*err = xstrdup("output error: failed to allocate HTTP batch buffer");
			}
			curl_global_cleanup();
			memset(&g_output, 0, sizeof(g_output));
			return -1;
		}
		return 0;
	}

	if (err) {
		char message[256];
		snprintf(message, sizeof(message), "output error: unknown output.type '%s'", cfg->output_type);
		*err = xstrdup(message);
	}
	return -1;
}

int output_get_mode(void)
{
	return (int)g_output.mode;
}

int output_emit_event(const char *json_line)
{
	fprintf(stderr, "syswatch: output_emit_event mode=%d\n", g_output.mode);
	char *copy;

	if (!json_line) {
		return -1;
	}

	switch (g_output.mode) {
	case OUTPUT_STDOUT:
		fputs(json_line, stdout);
		fputc('\n', stdout);
		fflush(stdout);
		return 0;
	case OUTPUT_FILE:
		if (!g_output.file_handle) {
			return -1;
		}
		fprintf(g_output.file_handle, "%s\n", json_line);
		fflush(g_output.file_handle);
		return 0;
	case OUTPUT_HTTP_POST:
		if (maybe_flush_http_batch() != 0) {
			return -1;
		}
		batch_start_if_needed();
		if (batch_ensure_capacity(g_output.batch_count + 1) != 0) {
			return -1;
		}
		copy = xstrdup(json_line);
		if (!copy) {
			return -1;
		}
		g_output.batch_items[g_output.batch_count++] = copy;
		if ((int)g_output.batch_count >= g_output.batch_size) {
			return http_post_batch();
		}
		if (g_output.batch_interval_seconds > 0 && batch_age_seconds() >= (double)g_output.batch_interval_seconds) {
			return http_post_batch();
		}
		return 0;
	}

	return -1;
}

int output_flush(void)
{
	if (g_output.mode == OUTPUT_FILE && g_output.file_handle) {
		return fflush(g_output.file_handle);
	}

	if (g_output.mode == OUTPUT_HTTP_POST) {
		return http_post_batch();
	}

	return 0;
}

void output_shutdown(void)
{
	output_flush();

	if (g_output.file_handle) {
		fclose(g_output.file_handle);
		g_output.file_handle = NULL;
	}

	if (g_output.batch_items) {
		free_batch_items();
		free(g_output.batch_items);
		g_output.batch_items = NULL;
	}

	if (g_output.curl_ready) {
		curl_global_cleanup();
		g_output.curl_ready = false;
	}

	memset(&g_output, 0, sizeof(g_output));
	g_output.mode = OUTPUT_STDOUT;
}
