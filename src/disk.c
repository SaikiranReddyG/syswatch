#include "syswatch.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define DISK_SECTOR_SIZE 512.0

static int is_alpha_only_suffix(const char *s)
{
	while (*s) {
		if (!isalpha((unsigned char)*s)) {
			return 0;
		}
		s++;
	}
	return 1;
}

static bool is_whole_disk_name(const char *name)
{
	if (!name || name[0] == '\0') {
		return false;
	}

	if (strncmp(name, "sd", 2) == 0 || strncmp(name, "hd", 2) == 0 || strncmp(name, "vd", 2) == 0) {
		return is_alpha_only_suffix(name + 2) != 0;
	}

	if (strncmp(name, "xvd", 3) == 0) {
		return is_alpha_only_suffix(name + 3) != 0;
	}

	if (strncmp(name, "nvme", 4) == 0) {
		int d1;
		int d2;
		char extra;
		if (sscanf(name, "nvme%dn%d%c", &d1, &d2, &extra) == 2) {
			return true;
		}
	}

	if (strncmp(name, "mmcblk", 6) == 0) {
		int d;
		char extra;
		if (sscanf(name, "mmcblk%d%c", &d, &extra) == 1) {
			return true;
		}
	}

	return false;
}

static int find_disk(const disk_snapshot_t *snap, const char *name)
{
	int i;
	for (i = 0; i < snap->count; i++) {
		if (strcmp(snap->items[i].name, name) == 0) {
			return i;
		}
	}
	return -1;
}

int disk_read_snapshot(disk_snapshot_t *out)
{
	FILE *fp;
	char line[512];

	if (!out) {
		return -1;
	}

	memset(out, 0, sizeof(*out));
	fp = fopen("/proc/diskstats", "r");
	if (!fp) {
		return -1;
	}

	while (fgets(line, sizeof(line), fp) && out->count < MAX_DISKS) {
		unsigned int major;
		unsigned int minor;
		char name[32];
		unsigned long long reads_completed;
		unsigned long long reads_merged;
		unsigned long long sectors_read;
		unsigned long long read_ms;
		unsigned long long writes_completed;
		unsigned long long writes_merged;
		unsigned long long sectors_written;
		unsigned long long write_ms;
		int n;

		n = sscanf(line,
			"%u %u %31s %llu %llu %llu %llu %llu %llu %llu %llu",
			&major,
			&minor,
			name,
			&reads_completed,
			&reads_merged,
			&sectors_read,
			&read_ms,
			&writes_completed,
			&writes_merged,
			&sectors_written,
			&write_ms);

		(void)major;
		(void)minor;
		(void)reads_completed;
		(void)reads_merged;
		(void)read_ms;
		(void)writes_completed;
		(void)writes_merged;
		(void)write_ms;

		if (n < 11 || !is_whole_disk_name(name)) {
			continue;
		}

		snprintf(out->items[out->count].name, sizeof(out->items[out->count].name), "%s", name);
		out->items[out->count].sectors_read = sectors_read;
		out->items[out->count].sectors_written = sectors_written;
		out->count++;
	}

	fclose(fp);
	return 0;
}

int disk_compute_stats(const disk_snapshot_t *prev, const disk_snapshot_t *curr, int interval_sec, disk_stats_t *out)
{
	int i;

	if (!prev || !curr || !out || interval_sec <= 0) {
		return -1;
	}

	memset(out, 0, sizeof(*out));

	for (i = 0; i < curr->count && out->count < MAX_DISKS; i++) {
		int idx;
		const disk_counter_t *c;
		const disk_counter_t *p;
		unsigned long long read_delta;
		unsigned long long write_delta;
		disk_rate_entry_t *dst;

		c = &curr->items[i];
		idx = find_disk(prev, c->name);
		if (idx < 0) {
			continue;
		}
		p = &prev->items[idx];

		read_delta = (c->sectors_read >= p->sectors_read) ? (c->sectors_read - p->sectors_read) : 0;
		write_delta = (c->sectors_written >= p->sectors_written) ? (c->sectors_written - p->sectors_written) : 0;

		dst = &out->items[out->count++];
		snprintf(dst->name, sizeof(dst->name), "%s", c->name);
		dst->read_bps = ((double)read_delta * DISK_SECTOR_SIZE) / (double)interval_sec;
		dst->write_bps = ((double)write_delta * DISK_SECTOR_SIZE) / (double)interval_sec;
	}

	return 0;
}
