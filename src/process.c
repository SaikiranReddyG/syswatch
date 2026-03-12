#include "syswatch.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	int pid;
	unsigned long long ticks;
} process_tick_entry_t;

static process_tick_entry_t g_prev_ticks[MAX_PROCESSES];
static int g_prev_tick_count = 0;
static unsigned long long g_prev_total_cpu = 0;
static int g_prev_initialized = 0;

static int is_pid_name(const char *name)
{
	const char *p;

	if (!name || *name == '\0') {
		return 0;
	}

	for (p = name; *p; p++) {
		if (!isdigit((unsigned char)*p)) {
			return 0;
		}
	}

	return 1;
}

static int read_total_cpu_jiffies(unsigned long long *out)
{
	FILE *fp;
	char line[512];
	unsigned long long user;
	unsigned long long nice;
	unsigned long long system;
	unsigned long long idle;
	unsigned long long iowait;
	unsigned long long irq;
	unsigned long long softirq;
	unsigned long long steal;

	if (!out) {
		return -1;
	}

	fp = fopen("/proc/stat", "r");
	if (!fp) {
		return -1;
	}

	if (!fgets(line, sizeof(line), fp)) {
		fclose(fp);
		return -1;
	}
	fclose(fp);

	if (sscanf(line,
		"cpu %llu %llu %llu %llu %llu %llu %llu %llu",
		&user,
		&nice,
		&system,
		&idle,
		&iowait,
		&irq,
		&softirq,
		&steal) < 4) {
		return -1;
	}

	*out = user + nice + system + idle + iowait + irq + softirq + steal;
	return 0;
}

static int find_prev_pid(int pid)
{
	int i;

	for (i = 0; i < g_prev_tick_count; i++) {
		if (g_prev_ticks[i].pid == pid) {
			return i;
		}
	}

	return -1;
}

static int read_process_stat(int pid, char *comm, size_t comm_len, unsigned long long *proc_ticks)
{
	char path[64];
	FILE *fp;
	char line[1024];
	char *lpar;
	char *rpar;
	char rest[1024];
	char *tokens[64];
	char *tok;
	int token_count;
	unsigned long long utime;
	unsigned long long stime;

	if (!comm || comm_len == 0 || !proc_ticks) {
		return -1;
	}

	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	fp = fopen(path, "r");
	if (!fp) {
		return -1;
	}

	if (!fgets(line, sizeof(line), fp)) {
		fclose(fp);
		return -1;
	}
	fclose(fp);

	lpar = strchr(line, '(');
	rpar = strrchr(line, ')');
	if (!lpar || !rpar || rpar <= lpar) {
		return -1;
	}

	*rpar = '\0';
	snprintf(comm, comm_len, "%s", lpar + 1);

	if (*(rpar + 1) == '\0') {
		return -1;
	}

	snprintf(rest, sizeof(rest), "%s", rpar + 2);
	token_count = 0;
	tok = strtok(rest, " ");
	while (tok && token_count < (int)(sizeof(tokens) / sizeof(tokens[0]))) {
		tokens[token_count++] = tok;
		tok = strtok(NULL, " ");
	}

	if (token_count <= 12) {
		return -1;
	}

	if (parse_ull(tokens[11], &utime) != 0 || parse_ull(tokens[12], &stime) != 0) {
		return -1;
	}

	*proc_ticks = utime + stime;
	return 0;
}

static int read_process_rss_bytes(int pid, unsigned long long *rss_bytes)
{
	char path[64];
	FILE *fp;
	char line[256];

	if (!rss_bytes) {
		return -1;
	}

	*rss_bytes = 0;
	snprintf(path, sizeof(path), "/proc/%d/status", pid);
	fp = fopen(path, "r");
	if (!fp) {
		return -1;
	}

	while (fgets(line, sizeof(line), fp)) {
		unsigned long long kb;
		if (sscanf(line, "VmRSS: %llu kB", &kb) == 1) {
			*rss_bytes = kb * 1024ULL;
			fclose(fp);
			return 0;
		}
	}

	fclose(fp);
	return 0;
}

static int cmp_cpu_desc(const void *a, const void *b)
{
	const process_entry_t *pa = (const process_entry_t *)a;
	const process_entry_t *pb = (const process_entry_t *)b;

	if (pa->cpu_pct < pb->cpu_pct) {
		return 1;
	}
	if (pa->cpu_pct > pb->cpu_pct) {
		return -1;
	}
	return 0;
}

static int cmp_mem_desc(const void *a, const void *b)
{
	const process_entry_t *pa = (const process_entry_t *)a;
	const process_entry_t *pb = (const process_entry_t *)b;

	if (pa->mem_bytes < pb->mem_bytes) {
		return 1;
	}
	if (pa->mem_bytes > pb->mem_bytes) {
		return -1;
	}
	return 0;
}

int process_read_list(process_list_t *out, process_sort_mode_t sort_mode, int limit)
{
	DIR *dir;
	struct dirent *ent;
	unsigned long long total_cpu;
	unsigned long long total_delta;
	process_tick_entry_t curr_ticks[MAX_PROCESSES];
	int curr_tick_count;

	if (!out) {
		return -1;
	}

	memset(out, 0, sizeof(*out));
	if (limit <= 0) {
		return 0;
	}
	if (limit > MAX_PROCESSES) {
		limit = MAX_PROCESSES;
	}

	if (read_total_cpu_jiffies(&total_cpu) != 0) {
		return -1;
	}
	total_delta = (g_prev_initialized && total_cpu >= g_prev_total_cpu) ? (total_cpu - g_prev_total_cpu) : 0;
	curr_tick_count = 0;

	dir = opendir("/proc");
	if (!dir) {
		return -1;
	}

	while ((ent = readdir(dir)) != NULL && out->count < MAX_PROCESSES) {
		int pid;
		unsigned long long proc_ticks;
		unsigned long long proc_delta;
		unsigned long long rss;
		int prev_idx;
		process_entry_t *dst;

		if (!is_pid_name(ent->d_name)) {
			continue;
		}

		pid = atoi(ent->d_name);
		if (pid <= 0) {
			continue;
		}

		dst = &out->items[out->count];
		if (read_process_stat(pid, dst->comm, sizeof(dst->comm), &proc_ticks) != 0) {
			continue;
		}
		if (read_process_rss_bytes(pid, &rss) != 0) {
			continue;
		}

		dst->pid = pid;
		dst->mem_bytes = rss;
		prev_idx = find_prev_pid(pid);
		proc_delta = 0;
		if (prev_idx >= 0 && proc_ticks >= g_prev_ticks[prev_idx].ticks) {
			proc_delta = proc_ticks - g_prev_ticks[prev_idx].ticks;
		}

		if (total_delta > 0) {
			dst->cpu_pct = 100.0 * ((double)proc_delta / (double)total_delta);
		} else {
			dst->cpu_pct = 0.0;
		}
		dst->cpu_pct = clamp_double(dst->cpu_pct, 0.0, 100.0);

		if (curr_tick_count < MAX_PROCESSES) {
			curr_ticks[curr_tick_count].pid = pid;
			curr_ticks[curr_tick_count].ticks = proc_ticks;
			curr_tick_count++;
		}
		out->count++;
	}

	closedir(dir);

	if (sort_mode == PROCESS_SORT_MEM) {
		qsort(out->items, (size_t)out->count, sizeof(out->items[0]), cmp_mem_desc);
	} else {
		qsort(out->items, (size_t)out->count, sizeof(out->items[0]), cmp_cpu_desc);
	}

	if (out->count > limit) {
		out->count = limit;
	}

	memcpy(g_prev_ticks, curr_ticks, sizeof(process_tick_entry_t) * (size_t)curr_tick_count);
	g_prev_tick_count = curr_tick_count;
	g_prev_total_cpu = total_cpu;
	g_prev_initialized = 1;

	return 0;
}
