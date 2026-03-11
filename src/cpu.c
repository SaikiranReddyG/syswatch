#include "syswatch.h"

#include <stdio.h>
#include <string.h>

static unsigned long long cpu_total_jiffies(const cpu_times_t *t)
{
	return t->user + t->nice + t->system + t->idle + t->iowait + t->irq + t->softirq + t->steal;
}

static void cpu_pct_from_delta(const cpu_times_t *prev, const cpu_times_t *curr, cpu_stats_t *out)
{
	unsigned long long prev_total;
	unsigned long long curr_total;
	unsigned long long total_delta;
	unsigned long long idle_delta;
	unsigned long long user_delta;
	unsigned long long sys_delta;

	prev_total = cpu_total_jiffies(prev);
	curr_total = cpu_total_jiffies(curr);
	total_delta = (curr_total >= prev_total) ? (curr_total - prev_total) : 0;
	idle_delta = (curr->idle >= prev->idle) ? (curr->idle - prev->idle) : 0;
	user_delta = (curr->user >= prev->user) ? (curr->user - prev->user) : 0;
	sys_delta = (curr->system >= prev->system) ? (curr->system - prev->system) : 0;

	if (total_delta == 0) {
		out->usage_pct = 0.0;
		out->user_pct = 0.0;
		out->system_pct = 0.0;
		out->idle_pct = 100.0;
		return;
	}

	out->idle_pct = 100.0 * ((double)idle_delta / (double)total_delta);
	out->usage_pct = 100.0 - out->idle_pct;
	out->user_pct = 100.0 * ((double)user_delta / (double)total_delta);
	out->system_pct = 100.0 * ((double)sys_delta / (double)total_delta);

	out->usage_pct = clamp_double(out->usage_pct, 0.0, 100.0);
	out->user_pct = clamp_double(out->user_pct, 0.0, 100.0);
	out->system_pct = clamp_double(out->system_pct, 0.0, 100.0);
	out->idle_pct = clamp_double(out->idle_pct, 0.0, 100.0);
}

int cpu_read_snapshot(cpu_snapshot_t *out)
{
	FILE *fp;
	char line[512];
	int core_count;

	if (!out) {
		return -1;
	}

	memset(out, 0, sizeof(*out));
	fp = fopen("/proc/stat", "r");
	if (!fp) {
		return -1;
	}

	core_count = 0;
	while (fgets(line, sizeof(line), fp)) {
		cpu_times_t parsed;
		int n;

		if (strncmp(line, "cpu ", 4) == 0) {
			n = sscanf(line,
				"cpu %llu %llu %llu %llu %llu %llu %llu %llu",
				&parsed.user,
				&parsed.nice,
				&parsed.system,
				&parsed.idle,
				&parsed.iowait,
				&parsed.irq,
				&parsed.softirq,
				&parsed.steal);
			if (n >= 4) {
				out->total = parsed;
			}
			continue;
		}

		if (strncmp(line, "cpu", 3) == 0 && line[3] >= '0' && line[3] <= '9' && core_count < MAX_CORES) {
			n = sscanf(line,
				"cpu%*d %llu %llu %llu %llu %llu %llu %llu %llu",
				&parsed.user,
				&parsed.nice,
				&parsed.system,
				&parsed.idle,
				&parsed.iowait,
				&parsed.irq,
				&parsed.softirq,
				&parsed.steal);
			if (n >= 4) {
				out->cores[core_count++] = parsed;
			}
			continue;
		}

		if (strncmp(line, "intr", 4) == 0) {
			break;
		}
	}

	out->core_count = core_count;
	fclose(fp);
	return 0;
}

int cpu_compute_stats(const cpu_snapshot_t *prev, const cpu_snapshot_t *curr, cpu_stats_t *out)
{
	int i;
	int core_count;

	if (!prev || !curr || !out) {
		return -1;
	}

	memset(out, 0, sizeof(*out));
	cpu_pct_from_delta(&prev->total, &curr->total, out);

	core_count = (prev->core_count < curr->core_count) ? prev->core_count : curr->core_count;
	out->core_count = core_count;

	for (i = 0; i < core_count; i++) {
		cpu_stats_t tmp;
		memset(&tmp, 0, sizeof(tmp));
		cpu_pct_from_delta(&prev->cores[i], &curr->cores[i], &tmp);
		out->core_usage_pct[i] = tmp.usage_pct;
	}

	return 0;
}
