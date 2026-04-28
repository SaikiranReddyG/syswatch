#define _POSIX_C_SOURCE 200809L

#include "syswatch.h"

#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef enum {
	SECTION_NONE = 0,
	SECTION_OUTPUT,
	SECTION_COLLECT,
	SECTION_LOG
} config_section_t;

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

static void set_error(char **err, const char *fmt, ...)
{
	va_list ap;
	int needed;
	char stackbuf[512];
	char *heapbuf;

	if (!err) {
		return;
	}

	va_start(ap, fmt);
	needed = vsnprintf(stackbuf, sizeof(stackbuf), fmt, ap);
	va_end(ap);

	if (needed < 0) {
		*err = xstrdup("config error");
		return;
	}

	if ((size_t)needed < sizeof(stackbuf)) {
		*err = xstrdup(stackbuf);
		return;
	}

	heapbuf = malloc((size_t)needed + 1);
	if (!heapbuf) {
		*err = xstrdup("config error: out of memory");
		return;
	}

	va_start(ap, fmt);
	vsnprintf(heapbuf, (size_t)needed + 1, fmt, ap);
	va_end(ap);
	*err = heapbuf;
}

static char *ltrim(char *text)
{
	while (text && (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')) {
		text++;
	}
	return text;
}

static void rtrim(char *text)
{
	size_t len;

	if (!text) {
		return;
	}

	len = strlen(text);
	while (len > 0) {
		char c = text[len - 1];
		if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
			break;
		}
		text[len - 1] = '\0';
		len--;
	}
}

static char *strip_inline_comment(char *text)
{
	bool in_quotes = false;
	char *cursor;

	if (!text) {
		return text;
	}

	for (cursor = text; *cursor != '\0'; cursor++) {
		if (*cursor == '"') {
			in_quotes = !in_quotes;
			continue;
		}
		if (!in_quotes && *cursor == '#') {
			if (cursor == text || isspace((unsigned char)cursor[-1])) {
				*cursor = '\0';
				break;
			}
		}
	}

	rtrim(text);
	return text;
}

static int unquote(char *text)
{
	size_t len;

	if (!text) {
		return 0;
	}

	len = strlen(text);
	if (len >= 2 && text[0] == '"' && text[len - 1] == '"') {
		memmove(text, text + 1, len - 2);
		text[len - 2] = '\0';
	}

	return 0;
}

static int parse_bool_value(const char *text, bool *out)
{
	if (!text || !out) {
		return -1;
	}
	if (strcmp(text, "true") == 0) {
		*out = true;
		return 0;
	}
	if (strcmp(text, "false") == 0) {
		*out = false;
		return 0;
	}
	return -1;
}

static int parse_int_value(const char *text, int min_value, int max_value, int *out)
{
	char *endptr;
	long parsed;

	if (!text || !out) {
		return -1;
	}

	errno = 0;
	parsed = strtol(text, &endptr, 10);
	if (errno != 0 || endptr == text || *endptr != '\0' || parsed < min_value || parsed > max_value) {
		return -1;
	}

	*out = (int)parsed;
	return 0;
}

static int set_scalar_top_level(syswatch_config_t *cfg, const char *key, char *value, char **err, int line_no)
{
	if (strcmp(key, "config_version") == 0) {
		if (strcmp(value, "1.0") != 0) {
			set_error(err, "config error at line %d: unsupported config_version '%s'", line_no, value);
			return -1;
		}
		strncpy(cfg->config_version, value, sizeof(cfg->config_version) - 1);
		return 0;
	}

	if (strcmp(key, "poll_interval_seconds") == 0) {
		if (parse_int_value(value, 1, 86400, &cfg->interval_sec) != 0) {
			set_error(err, "config error at line %d: poll_interval_seconds must be a positive integer, got '%s'", line_no, value);
			return -1;
		}
		return 0;
	}

	if (strcmp(key, "host_override") == 0) {
		strncpy(cfg->host_override, value, sizeof(cfg->host_override) - 1);
		return 0;
	}

	set_error(err, "config error at line %d: unknown key '%s'", line_no, key);
	return -1;
}

static int set_output_key(syswatch_config_t *cfg, const char *key, char *value, char **err, int line_no)
{
	if (strcmp(key, "type") == 0) {
		if (strcmp(value, "stdout") != 0 && strcmp(value, "file") != 0 && strcmp(value, "http_post") != 0) {
			set_error(err, "config error at line %d: output.type must be stdout, file, or http_post, got '%s'", line_no, value);
			return -1;
		}
		strncpy(cfg->output_type, value, sizeof(cfg->output_type) - 1);
		return 0;
	}
	if (strcmp(key, "url") == 0) {
		strncpy(cfg->output_url, value, sizeof(cfg->output_url) - 1);
		return 0;
	}
	if (strcmp(key, "path") == 0) {
		strncpy(cfg->output_path, value, sizeof(cfg->output_path) - 1);
		return 0;
	}
	if (strcmp(key, "batch_size") == 0) {
		if (parse_int_value(value, 1, 100000, &cfg->output_batch_size) != 0) {
			set_error(err, "config error at line %d: batch_size must be a positive integer, got '%s'", line_no, value);
			return -1;
		}
		return 0;
	}
	if (strcmp(key, "batch_interval_seconds") == 0) {
		if (parse_int_value(value, 1, 86400, &cfg->output_batch_interval_seconds) != 0) {
			set_error(err, "config error at line %d: batch_interval_seconds must be a positive integer, got '%s'", line_no, value);
			return -1;
		}
		return 0;
	}
	if (strcmp(key, "retry_max_attempts") == 0) {
		if (parse_int_value(value, 0, 100, &cfg->output_retry_max_attempts) != 0) {
			set_error(err, "config error at line %d: retry_max_attempts must be a non-negative integer, got '%s'", line_no, value);
			return -1;
		}
		return 0;
	}
	if (strcmp(key, "retry_backoff_seconds") == 0) {
		if (parse_int_value(value, 0, 3600, &cfg->output_retry_backoff_seconds) != 0) {
			set_error(err, "config error at line %d: retry_backoff_seconds must be a non-negative integer, got '%s'", line_no, value);
			return -1;
		}
		return 0;
	}

	set_error(err, "config error at line %d: unknown output key '%s'", line_no, key);
	return -1;
}

static int set_collect_key(syswatch_config_t *cfg, const char *key, char *value, char **err, int line_no)
{
	bool flag;

	if (parse_bool_value(value, &flag) != 0) {
		set_error(err, "config error at line %d: collect.%s must be true or false, got '%s'", line_no, key, value);
		return -1;
	}

	if (strcmp(key, "cpu") == 0) {
		cfg->show_cpu = flag;
		return 0;
	}
	if (strcmp(key, "memory") == 0) {
		cfg->show_memory = flag;
		return 0;
	}
	if (strcmp(key, "disk") == 0) {
		cfg->show_disk = flag;
		return 0;
	}
	if (strcmp(key, "network") == 0) {
		cfg->show_network = flag;
		return 0;
	}
	if (strcmp(key, "load") == 0) {
		return 0;
	}
	if (strcmp(key, "processes") == 0) {
		cfg->show_processes = flag;
		return 0;
	}

	set_error(err, "config error at line %d: unknown collect key '%s'", line_no, key);
	return -1;
}

static int set_log_key(syswatch_config_t *cfg, const char *key, char *value, char **err, int line_no)
{
	if (strcmp(key, "level") == 0) {
		if (strcmp(value, "debug") != 0 && strcmp(value, "info") != 0 && strcmp(value, "warn") != 0 && strcmp(value, "error") != 0) {
			set_error(err, "config error at line %d: log.level must be debug, info, warn, or error, got '%s'", line_no, value);
			return -1;
		}
		strncpy(cfg->log_level, value, sizeof(cfg->log_level) - 1);
		return 0;
	}
	if (strcmp(key, "destination") == 0) {
		if (strcmp(value, "stderr") != 0 && strcmp(value, "file") != 0) {
			set_error(err, "config error at line %d: log.destination must be stderr or file, got '%s'", line_no, value);
			return -1;
		}
		strncpy(cfg->log_destination, value, sizeof(cfg->log_destination) - 1);
		return 0;
	}
	if (strcmp(key, "path") == 0) {
		strncpy(cfg->log_path, value, sizeof(cfg->log_path) - 1);
		return 0;
	}

	set_error(err, "config error at line %d: unknown log key '%s'", line_no, key);
	return -1;
}

int load_config_file(const char *path, syswatch_config_t *cfg, char **err)
{
	FILE *file;
	char line[1024];
	int line_no;
	config_section_t section;

	if (!cfg) {
		set_error(err, "config error: null config");
		return -1;
	}

	file = NULL;
	if (!path || strcmp(path, "-") == 0) {
		file = stdin;
	} else {
		file = fopen(path, "r");
		if (!file) {
			set_error(err, "config error: failed to open '%s'", path);
			return -1;
		}
	}

	line_no = 0;
	section = SECTION_NONE;

	while (fgets(line, sizeof(line), file)) {
		char *cursor;
		char *key;
		char *value;
		int indent_spaces;

		line_no++;
		cursor = line;
		cursor = ltrim(cursor);
		if (*cursor == '\0' || *cursor == '\n' || *cursor == '#') {
			continue;
		}

		indent_spaces = 0;
		for (cursor = line; *cursor == ' '; cursor++) {
			indent_spaces++;
		}

		if (indent_spaces != 0 && indent_spaces != 2) {
			set_error(err, "config error at line %d: expected 0 or 2 leading spaces, got %d", line_no, indent_spaces);
			goto fail;
		}

		cursor = line + indent_spaces;
		cursor = strip_inline_comment(cursor);
		cursor = ltrim(cursor);
		rtrim(cursor);
		if (*cursor == '\0') {
			continue;
		}

		key = cursor;
		value = strchr(cursor, ':');
		if (!value) {
			set_error(err, "config error at line %d: expected key: value", line_no);
			goto fail;
		}

		*value = '\0';
		value++;
		key = ltrim(key);
		rtrim(key);
		value = ltrim(value);
		rtrim(value);
		unquote(key);
		unquote(value);

		if (indent_spaces == 0) {
			if (*value == '\0' && strcmp(key, "output") == 0) {
				section = SECTION_OUTPUT;
				continue;
			}
			if (*value == '\0' && strcmp(key, "collect") == 0) {
				section = SECTION_COLLECT;
				continue;
			}
			if (*value == '\0' && strcmp(key, "log") == 0) {
				section = SECTION_LOG;
				continue;
			}

			section = SECTION_NONE;
			if (set_scalar_top_level(cfg, key, value, err, line_no) != 0) {
				goto fail;
			}
			continue;
		}

		if (section == SECTION_NONE) {
			set_error(err, "config error at line %d: nested key '%s' has no active section", line_no, key);
			goto fail;
		}

		if (section == SECTION_OUTPUT) {
			if (set_output_key(cfg, key, value, err, line_no) != 0) {
				goto fail;
			}
		} else if (section == SECTION_COLLECT) {
			if (set_collect_key(cfg, key, value, err, line_no) != 0) {
				goto fail;
			}
		} else if (section == SECTION_LOG) {
			if (set_log_key(cfg, key, value, err, line_no) != 0) {
				goto fail;
			}
		}
	}

	if (file != stdin) {
		fclose(file);
	}
	return 0;

fail:
	if (file != stdin) {
		fclose(file);
	}
	return -1;
}

int validate_config(const syswatch_config_t *cfg, char **err)
{
	if (!cfg) {
		set_error(err, "config error: null config");
		return -1;
	}

	if (strcmp(cfg->config_version, "1.0") != 0) {
		set_error(err, "config error: config_version must be '1.0', got '%s'", cfg->config_version);
		return -1;
	}

	if (cfg->interval_sec <= 0) {
		set_error(err, "config error: poll_interval_seconds must be a positive integer");
		return -1;
	}

	if (cfg->output_type[0] == '\0') {
		set_error(err, "config error: output.type is required");
		return -1;
	}

	if (strcmp(cfg->output_type, "file") == 0 && cfg->output_path[0] == '\0') {
		set_error(err, "config error: output.path required for file output");
		return -1;
	}

	if (strcmp(cfg->output_type, "http_post") == 0) {
		if (cfg->output_url[0] == '\0') {
			set_error(err, "config error: output.url required for http_post");
			return -1;
		}
		if (cfg->output_batch_size <= 0) {
			set_error(err, "config error: output.batch_size must be positive for http_post");
			return -1;
		}
		if (cfg->output_batch_interval_seconds <= 0) {
			set_error(err, "config error: output.batch_interval_seconds must be positive for http_post");
			return -1;
		}
	}

	if (cfg->log_level[0] != '\0' && strcmp(cfg->log_level, "debug") != 0 && strcmp(cfg->log_level, "info") != 0 && strcmp(cfg->log_level, "warn") != 0 && strcmp(cfg->log_level, "error") != 0) {
		set_error(err, "config error: invalid log.level '%s'", cfg->log_level);
		return -1;
	}

	if (cfg->log_destination[0] != '\0' && strcmp(cfg->log_destination, "stderr") != 0 && strcmp(cfg->log_destination, "file") != 0) {
		set_error(err, "config error: invalid log.destination '%s'", cfg->log_destination);
		return -1;
	}

	return 0;
}
