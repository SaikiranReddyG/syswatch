#define _POSIX_C_SOURCE 200809L

#include "syswatch.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

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

/* Get monotonic time (for interval calculations, immune to clock adjustments) */
int get_mono_time(struct timespec *ts)
{
	if (!ts) {
		return -1;
	}
	if (clock_gettime(CLOCK_MONOTONIC, ts) != 0) {
		return -1;
	}
	return 0;
}

/* Get wall-clock time (for timestamps in events) */
int get_wall_time(struct timespec *ts)
{
	if (!ts) {
		return -1;
	}
	if (clock_gettime(CLOCK_REALTIME, ts) != 0) {
		return -1;
	}
	return 0;
}

/* Format timespec as RFC 3339 with timezone offset */
void format_rfc3339(struct timespec *ts, char *buf, size_t len)
{
	struct tm *tm_now;
	char iso_buf[32];
	char tz_buf[16];
	time_t sec;

	if (!buf || len == 0) {
		return;
	}

	if (!ts) {
		snprintf(buf, len, "1970-01-01T00:00:00Z");
		return;
	}

	sec = ts->tv_sec;
	tm_now = localtime(&sec);
	if (!tm_now) {
		snprintf(buf, len, "1970-01-01T00:00:00Z");
		return;
	}

	/* Format as ISO 8601: YYYY-MM-DDTHH:MM:SS.mmm+HH:MM */
	strftime(iso_buf, sizeof(iso_buf), "%Y-%m-%dT%H:%M:%S", tm_now);

	/* Timezone offset (use __tm_gmtoff on Linux glibc) */
	long offset = tm_now->__tm_gmtoff;
	int hours = offset / 3600;
	int minutes = (labs(offset) % 3600) / 60;
	snprintf(tz_buf, sizeof(tz_buf), "%+03d:%02d", hours, minutes);

	/* Combine with nanoseconds to milliseconds */
	unsigned int msec = ts->tv_nsec / 1000000;
	snprintf(buf, len, "%s.%03u%s", iso_buf, msec, tz_buf);
}

/* Calculate delta between two timespec values in seconds */
double timespec_delta_seconds(struct timespec *prev, struct timespec *curr)
{
	if (!prev || !curr) {
		return 0.0;
	}

	long sec_diff = curr->tv_sec - prev->tv_sec;
	long nsec_diff = curr->tv_nsec - prev->tv_nsec;

	double delta = (double)sec_diff + (double)nsec_diff / 1e9;
	return delta > 0.0 ? delta : 0.0;
}

/* Rolling statistics sliding buffer */
void rolling_stat_add(rolling_stat_t *stat, double val)
{
	if (!stat) return;
	stat->samples[stat->head] = val;
	stat->head = (stat->head + 1) % ANOMALY_WINDOW;
	if (stat->count < ANOMALY_WINDOW) {
		stat->count++;
	}
}

bool rolling_stat_check(const rolling_stat_t *stat, double val, double *mean_out, double *stddev_out)
{
	if (!stat || stat->count < 10) { /* Need a baseline cache size to measure standard deviations realistically */
		return false;
	}

	double sum = 0.0;
	for (int i = 0; i < stat->count; i++) {
		sum += stat->samples[i];
	}
	double mean = sum / stat->count;

	double var_sum = 0.0;
	for (int i = 0; i < stat->count; i++) {
		double diff = stat->samples[i] - mean;
		var_sum += diff * diff;
	}
	double variance = var_sum / stat->count;
	double stddev = sqrt(variance);

	if (mean_out) *mean_out = mean;
	if (stddev_out) *stddev_out = stddev;

	if (stddev < 0.001) { /* Skip flat lines */
		return false;
	}

	return (val > mean + (3.0 * stddev));
}

