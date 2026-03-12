#include "syswatch.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void handle_sigint(int sig)
{
	(void)sig;
	g_stop = 1;
}

static int parse_positive_int(const char *s, int *out)
{
	long v;
	char *endptr;

	if (!s || !out) {
		return -1;
	}

	errno = 0;
	v = strtol(s, &endptr, 10);
	if (errno != 0 || endptr == s || *endptr != '\0' || v <= 0 || v > 86400) {
		return -1;
	}

	*out = (int)v;
	return 0;
}

static int parse_non_negative_int(const char *s, int *out)
{
	long v;
	char *endptr;

	if (!s || !out) {
		return -1;
	}

	errno = 0;
	v = strtol(s, &endptr, 10);
	if (errno != 0 || endptr == s || *endptr != '\0' || v < 0 || v > 1000000) {
		return -1;
	}

	*out = (int)v;
	return 0;
}

static int sleep_interval(int seconds)
{
	unsigned int remaining;

	remaining = (unsigned int)seconds;
	while (remaining > 0) {
		remaining = sleep(remaining);
		if (g_stop) {
			return -1;
		}
	}

	return 0;
}

void init_default_config(syswatch_config_t *cfg)
{
	if (!cfg) {
		return;
	}

	memset(cfg, 0, sizeof(*cfg));
	cfg->interval_sec = DEFAULT_REFRESH_INTERVAL;
	cfg->iterations = DEFAULT_ITERATIONS;
	cfg->csv_mode = false;

	cfg->show_cpu = true;
	cfg->show_memory = true;
	cfg->show_disk = true;
	cfg->show_network = true;
	cfg->show_processes = false;
	cfg->show_disk_details = false;
	cfg->show_network_details = false;

	cfg->include_loopback = false;
	cfg->process_sort = PROCESS_SORT_CPU;
	cfg->top_n = DEFAULT_TOP_N;
}

void print_usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("  -i, --interval SEC      Refresh interval in seconds (default: 1)\n");
	printf("  -n, --iterations N      Number of rows to print, 0 = infinite (default: 0)\n");
	printf("  -c, --csv               CSV mode (no ANSI colors)\n");
	printf("      --no-cpu            Hide CPU columns\n");
	printf("      --no-memory         Hide memory columns\n");
	printf("      --no-disk           Hide disk columns\n");
	printf("      --no-network        Hide network columns\n");
	printf("      --disk-details      Show per-disk throughput breakdown\n");
	printf("      --net-details       Show per-interface throughput breakdown\n");
	printf("      --include-lo        Include loopback interface in network stats\n");
	printf("  -p, --processes         Show top processes column\n");
	printf("  -t, --top N             Number of top processes to include (default: 5)\n");
	printf("  -s, --proc-sort MODE    Process sort mode: cpu or mem (default: cpu)\n");
	printf("  -h, --help              Show this help text\n");
}

int parse_args(int argc, char **argv, syswatch_config_t *cfg)
{
	int opt;
	int long_idx;
	static struct option long_opts[] = {
		{"interval", required_argument, NULL, 'i'},
		{"iterations", required_argument, NULL, 'n'},
		{"csv", no_argument, NULL, 'c'},
		{"no-cpu", no_argument, NULL, 1000},
		{"no-memory", no_argument, NULL, 1001},
		{"no-disk", no_argument, NULL, 1002},
		{"no-network", no_argument, NULL, 1003},
		{"disk-details", no_argument, NULL, 1005},
		{"net-details", no_argument, NULL, 1006},
		{"include-lo", no_argument, NULL, 1004},
		{"processes", no_argument, NULL, 'p'},
		{"top", required_argument, NULL, 't'},
		{"proc-sort", required_argument, NULL, 's'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0}
	};

	if (!cfg) {
		return -1;
	}

	while ((opt = getopt_long(argc, argv, "i:n:cpt:s:h", long_opts, &long_idx)) != -1) {
		(void)long_idx;
		switch (opt) {
		case 'i':
			if (parse_positive_int(optarg, &cfg->interval_sec) != 0) {
				fprintf(stderr, "Invalid interval: %s\n", optarg);
				return -1;
			}
			break;
		case 'n':
			if (parse_non_negative_int(optarg, &cfg->iterations) != 0) {
				fprintf(stderr, "Invalid iterations: %s\n", optarg);
				return -1;
			}
			break;
		case 'c':
			cfg->csv_mode = true;
			break;
		case 'p':
			cfg->show_processes = true;
			break;
		case 't':
			if (parse_positive_int(optarg, &cfg->top_n) != 0) {
				fprintf(stderr, "Invalid top value: %s\n", optarg);
				return -1;
			}
			break;
		case 's':
			if (strcmp(optarg, "cpu") == 0) {
				cfg->process_sort = PROCESS_SORT_CPU;
			} else if (strcmp(optarg, "mem") == 0) {
				cfg->process_sort = PROCESS_SORT_MEM;
			} else {
				fprintf(stderr, "Invalid process sort mode: %s\n", optarg);
				return -1;
			}
			break;
		case 'h':
			print_usage(argv[0]);
			exit(0);
		case 1000:
			cfg->show_cpu = false;
			break;
		case 1001:
			cfg->show_memory = false;
			break;
		case 1002:
			cfg->show_disk = false;
			break;
		case 1003:
			cfg->show_network = false;
			break;
		case 1004:
			cfg->include_loopback = true;
			break;
		case 1005:
			cfg->show_disk_details = true;
			break;
		case 1006:
			cfg->show_network_details = true;
			break;
		default:
			return -1;
		}
	}

	if (!cfg->show_cpu && !cfg->show_memory && !cfg->show_disk && !cfg->show_network && !cfg->show_processes) {
		fprintf(stderr, "All columns are disabled. Enable at least one metric.\n");
		return -1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	syswatch_config_t cfg;
	bool need_baseline;
	bool need_sleep_before_row;
	bool cpu_ready;
	bool disk_ready;
	bool net_ready;
	int rows;

	cpu_snapshot_t cpu_prev;
	cpu_snapshot_t cpu_curr;
	cpu_stats_t cpu_stats;

	disk_snapshot_t disk_prev;
	disk_snapshot_t disk_curr;
	disk_stats_t disk_stats;

	net_snapshot_t net_prev;
	net_snapshot_t net_curr;
	net_stats_t net_stats;

	memory_stats_t mem_stats;
	process_list_t proc_list;

	init_default_config(&cfg);
	if (parse_args(argc, argv, &cfg) != 0) {
		print_usage(argv[0]);
		return 1;
	}

	signal(SIGINT, handle_sigint);

	memset(&cpu_prev, 0, sizeof(cpu_prev));
	memset(&cpu_curr, 0, sizeof(cpu_curr));
	memset(&disk_prev, 0, sizeof(disk_prev));
	memset(&disk_curr, 0, sizeof(disk_curr));
	memset(&net_prev, 0, sizeof(net_prev));
	memset(&net_curr, 0, sizeof(net_curr));

	need_baseline = cfg.show_cpu || cfg.show_disk || cfg.show_network;
	cpu_ready = false;
	disk_ready = false;
	net_ready = false;

	if (cfg.show_cpu && cpu_read_snapshot(&cpu_prev) == 0) {
		cpu_ready = true;
	}
	if (cfg.show_disk && disk_read_snapshot(&disk_prev) == 0) {
		disk_ready = true;
	}
	if (cfg.show_network && net_read_snapshot(&net_prev, cfg.include_loopback) == 0) {
		net_ready = true;
	}

	display_print_header(&cfg);

	rows = 0;
	need_sleep_before_row = need_baseline;

	while (!g_stop && (cfg.iterations == 0 || rows < cfg.iterations)) {
		const cpu_stats_t *cpu_ptr;
		const memory_stats_t *mem_ptr;
		const disk_stats_t *disk_ptr;
		const net_stats_t *net_ptr;
		const process_list_t *proc_ptr;

		cpu_ptr = NULL;
		mem_ptr = NULL;
		disk_ptr = NULL;
		net_ptr = NULL;
		proc_ptr = NULL;

		if (need_sleep_before_row) {
			if (sleep_interval(cfg.interval_sec) != 0) {
				break;
			}
		}

		if (cfg.show_cpu && cpu_read_snapshot(&cpu_curr) == 0) {
			if (cpu_ready && cpu_compute_stats(&cpu_prev, &cpu_curr, &cpu_stats) == 0) {
				cpu_ptr = &cpu_stats;
			}
			cpu_prev = cpu_curr;
			cpu_ready = true;
		}

		if (cfg.show_memory && memory_read_stats(&mem_stats) == 0) {
			mem_ptr = &mem_stats;
		}

		if (cfg.show_disk && disk_read_snapshot(&disk_curr) == 0) {
			if (disk_ready && disk_compute_stats(&disk_prev, &disk_curr, cfg.interval_sec, &disk_stats) == 0) {
				disk_ptr = &disk_stats;
			}
			disk_prev = disk_curr;
			disk_ready = true;
		}

		if (cfg.show_network && net_read_snapshot(&net_curr, cfg.include_loopback) == 0) {
			if (net_ready && net_compute_stats(&net_prev, &net_curr, cfg.interval_sec, &net_stats) == 0) {
				net_ptr = &net_stats;
			}
			net_prev = net_curr;
			net_ready = true;
		}

		if (cfg.show_processes && process_read_list(&proc_list, cfg.process_sort, cfg.top_n) == 0) {
			proc_ptr = &proc_list;
		}

		display_print_row(&cfg, cpu_ptr, mem_ptr, disk_ptr, net_ptr, proc_ptr);
		fflush(stdout);

		rows++;
		need_sleep_before_row = true;
	}

	return 0;
}
