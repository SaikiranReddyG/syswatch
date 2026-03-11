#include "syswatch.h"

#include <stdio.h>
#include <string.h>

int memory_read_stats(memory_stats_t *out)
{
	FILE *fp;
	char line[256];

	if (!out) {
		return -1;
	}

	memset(out, 0, sizeof(*out));
	fp = fopen("/proc/meminfo", "r");
	if (!fp) {
		return -1;
	}

	while (fgets(line, sizeof(line), fp)) {
		char key[64];
		unsigned long long value_kb;

		if (sscanf(line, "%63[^:]: %llu kB", key, &value_kb) != 2) {
			continue;
		}

		if (strcmp(key, "MemTotal") == 0) {
			out->mem_total = value_kb * 1024ULL;
		} else if (strcmp(key, "MemAvailable") == 0) {
			out->mem_available = value_kb * 1024ULL;
		} else if (strcmp(key, "MemFree") == 0) {
			out->mem_free = value_kb * 1024ULL;
		} else if (strcmp(key, "Buffers") == 0) {
			out->buffers = value_kb * 1024ULL;
		} else if (strcmp(key, "Cached") == 0) {
			out->cached = value_kb * 1024ULL;
		} else if (strcmp(key, "SwapTotal") == 0) {
			out->swap_total = value_kb * 1024ULL;
		} else if (strcmp(key, "SwapFree") == 0) {
			out->swap_free = value_kb * 1024ULL;
		}
	}

	fclose(fp);

	if (out->mem_total >= out->mem_available) {
		out->mem_used = out->mem_total - out->mem_available;
	}
	if (out->swap_total >= out->swap_free) {
		out->swap_used = out->swap_total - out->swap_free;
	}

	return 0;
}
