#include "syswatch.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int read_first_line(const char *path, char *buf, size_t size)
{
	FILE *fp;

	if (!path || !buf || size == 0) {
		return -1;
	}

	fp = fopen(path, "r");
	if (!fp) {
		return -1;
	}

	if (!fgets(buf, (int)size, fp)) {
		fclose(fp);
		return -1;
	}

	fclose(fp);
	return 0;
}

char *trim_whitespace(char *s)
{
	char *end;

	if (!s) {
		return NULL;
	}

	while (*s && isspace((unsigned char)*s)) {
		s++;
	}

	if (*s == '\0') {
		return s;
	}

	end = s + strlen(s) - 1;
	while (end > s && isspace((unsigned char)*end)) {
		*end = '\0';
		end--;
	}

	return s;
}

int parse_ull(const char *s, unsigned long long *out)
{
	char *endptr;
	unsigned long long val;

	if (!s || !out) {
		return -1;
	}

	errno = 0;
	val = strtoull(s, &endptr, 10);
	if (errno != 0 || endptr == s) {
		return -1;
	}

	while (*endptr) {
		if (!isspace((unsigned char)*endptr)) {
			return -1;
		}
		endptr++;
	}

	*out = val;
	return 0;
}

double clamp_double(double val, double min, double max)
{
	if (val < min) {
		return min;
	}
	if (val > max) {
		return max;
	}
	return val;
}

void format_bytes(double bytes, char *buf, size_t len)
{
	static const char *units[] = {"B", "K", "M", "G", "T"};
	double value;
	int unit_idx;

	if (!buf || len == 0) {
		return;
	}

	value = bytes;
	unit_idx = 0;
	while (value >= 1024.0 && unit_idx < 4) {
		value /= 1024.0;
		unit_idx++;
	}

	if (unit_idx == 0) {
		snprintf(buf, len, "%.0f%s", value, units[unit_idx]);
	} else {
		snprintf(buf, len, "%.1f%s", value, units[unit_idx]);
	}
}

void format_timestamp(char *buf, size_t len)
{
	time_t now;
	struct tm *tm_now;

	if (!buf || len == 0) {
		return;
	}

	now = time(NULL);
	tm_now = localtime(&now);
	if (tm_now == NULL) {
		snprintf(buf, len, "--:--:--");
		return;
	}

	strftime(buf, len, "%H:%M:%S", tm_now);
}
