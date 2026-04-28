#define _POSIX_C_SOURCE 200809L
#include "syswatch.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <netdb.h>

static volatile sig_atomic_t g_stop = 0;
static syswatch_config_t *g_cfg = NULL;  /* Global config for signal handler and threads */
static pthread_t g_collector_thread;
static pthread_t g_delivery_thread;

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

	/* Flush and shutdown output cleanly */
	/* Flush output buffers but keep the backend active (don't shutdown),
	 * so stateful backends like http_post retain their configuration
	 * across sleep intervals. */
	output_flush();

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
	strncpy(cfg->config_version, "1.0", sizeof(cfg->config_version) - 1);
	strncpy(cfg->output_type, "stdout", sizeof(cfg->output_type) - 1);
	strncpy(cfg->log_level, "info", sizeof(cfg->log_level) - 1);
	strncpy(cfg->log_destination, "stderr", sizeof(cfg->log_destination) - 1);

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
	cfg->config_path[0] = '\0';
	cfg->validate_only = false;
	
	/* Initialize event queue and collector state */
	cfg->event_queue = queue_create();
	memset(&cfg->collector_state, 0, sizeof(cfg->collector_state));
	cfg->collector_state.has_previous_sample = false;
	memset(&cfg->internal_metrics, 0, sizeof(cfg->internal_metrics));
}

/* Collector thread: reads metrics continuously and enqueues JSON events */
static void *collector_thread(void *arg)
{
	syswatch_config_t *cfg = (syswatch_config_t *)arg;
	if (!cfg) {
		return NULL;
	}

	cpu_snapshot_t cpu_prev, cpu_curr;
	cpu_stats_t cpu_stats;
	disk_snapshot_t disk_prev, disk_curr;
	disk_stats_t disk_stats;
	net_snapshot_t net_prev, net_curr;
	net_stats_t net_stats;
	memory_stats_t mem_stats;
	process_list_t proc_list;

	bool cpu_ready = false, disk_ready = false, net_ready = false;
	int rows = 0;
	bool first_iteration = true;
	struct timespec last_mono;
	struct timespec mono_now;

	memset(&cpu_prev, 0, sizeof(cpu_prev));
	memset(&cpu_curr, 0, sizeof(cpu_curr));
	memset(&disk_prev, 0, sizeof(disk_prev));
	memset(&disk_curr, 0, sizeof(disk_curr));
	memset(&net_prev, 0, sizeof(net_prev));
	memset(&net_curr, 0, sizeof(net_curr));

	/* Emit warm-up event */
	{
		char json[512];
		struct timespec ts;
		char rfc_ts[64];
		char host_json[256];
		char hostbuf[128];

		get_wall_time(&ts);
		format_rfc3339(&ts, rfc_ts, sizeof(rfc_ts));

		if (cfg->host_override[0] != '\0') {
			strncpy(hostbuf, cfg->host_override, sizeof(hostbuf)-1);
			hostbuf[sizeof(hostbuf)-1] = '\0';
		} else {
			if (gethostname(hostbuf, sizeof(hostbuf)) != 0) {
				strncpy(hostbuf, "unknown", sizeof(hostbuf));
				hostbuf[sizeof(hostbuf)-1] = '\0';
			}
		}
		json_escape_string(hostbuf, host_json, sizeof(host_json));

		snprintf(json, sizeof(json),
			"{\"timestamp\":\"%s\",\"host\":%s,\"source\":\"syswatch\",\"event_type\":\"syswatch.lifecycle\",\"severity\":\"info\",\"payload\":{\"state\":\"warming_up\"}}",
			rfc_ts, host_json);
		queue_enqueue(cfg->event_queue, json, strlen(json));
	}

	/* Read baseline samples */
	if (cfg->show_cpu && cpu_read_snapshot(&cpu_prev) == 0) {
		cpu_ready = true;
	}
	if (cfg->show_disk && disk_read_snapshot(&disk_prev) == 0) {
		disk_ready = true;
	}
	if (cfg->show_network && net_read_snapshot(&net_prev, cfg->include_loopback) == 0) {
		net_ready = true;
	}
	get_mono_time(&last_mono);

	int current_interval = cfg->interval_sec;
	bool high_res_active = false;
	double high_res_remaining = 0.0;

	rolling_stat_t cpu_rolling;
	memset(&cpu_rolling, 0, sizeof(cpu_rolling));

	while (!g_stop && (cfg->iterations == 0 || rows < cfg->iterations)) {
		/* Sleep for configured interval */
		if (sleep(current_interval) != 0 && g_stop) {
			break;
		}

		get_mono_time(&mono_now);
		double delta_sec = timespec_delta_seconds(&last_mono, &mono_now);
		last_mono = mono_now;

		/* Emit "ready" event on first real iteration */
		if (first_iteration) {
			char json[512];
			struct timespec ts;
			char rfc_ts[64];
			char host_json[256];
			char hostbuf[128];

			get_wall_time(&ts);
			format_rfc3339(&ts, rfc_ts, sizeof(rfc_ts));

			if (cfg->host_override[0] != '\0') {
				strncpy(hostbuf, cfg->host_override, sizeof(hostbuf)-1);
				hostbuf[sizeof(hostbuf)-1] = '\0';
			} else {
				if (gethostname(hostbuf, sizeof(hostbuf)) != 0) {
					strncpy(hostbuf, "unknown", sizeof(hostbuf));
					hostbuf[sizeof(hostbuf)-1] = '\0';
				}
			}
			json_escape_string(hostbuf, host_json, sizeof(host_json));

			snprintf(json, sizeof(json),
				"{\"timestamp\":\"%s\",\"host\":%s,\"source\":\"syswatch\",\"event_type\":\"syswatch.lifecycle\",\"severity\":\"info\",\"payload\":{\"state\":\"ready\"}}",
				rfc_ts, host_json);
			queue_enqueue(cfg->event_queue, json, strlen(json));
			first_iteration = false;
		}

		cfg->collector_state.has_previous_sample = true;

		/* Collect metrics */
		const cpu_stats_t *cpu_ptr = NULL;
		const memory_stats_t *mem_ptr = NULL;
		const disk_stats_t *disk_ptr = NULL;
		const net_stats_t *net_ptr = NULL;
		const process_list_t *proc_ptr = NULL;

		if (cfg->show_cpu && cpu_read_snapshot(&cpu_curr) == 0) {
			if (cpu_ready && cpu_compute_stats(&cpu_prev, &cpu_curr, &cpu_stats) == 0) {
				cpu_ptr = &cpu_stats;
			}
			cpu_prev = cpu_curr;
			cpu_ready = true;
		}

		if (cfg->show_memory && memory_read_stats(&mem_stats) == 0) {
			mem_ptr = &mem_stats;
		}

		if (cfg->show_disk && disk_read_snapshot(&disk_curr) == 0) {
			if (disk_ready && disk_compute_stats(&disk_prev, &disk_curr, cfg->interval_sec, &disk_stats) == 0) {
				disk_ptr = &disk_stats;
			}
			disk_prev = disk_curr;
			disk_ready = true;
		}

		if (cfg->show_network && net_read_snapshot(&net_curr, cfg->include_loopback) == 0) {
			if (net_ready && net_compute_stats(&net_prev, &net_curr, cfg->interval_sec, &net_stats) == 0) {
				net_ptr = &net_stats;
			}
			net_prev = net_curr;
			net_ready = true;
		}

		if (cfg->show_processes && process_read_list(&proc_list, cfg->process_sort, cfg->top_n) == 0) {
			proc_ptr = &proc_list;
		}

		/* Emit JSON events for each metric category */
		{
			struct timespec ts;
			char rfc_ts[64];
			char hostbuf[128];
			char host_json[256];

			get_wall_time(&ts);
			format_rfc3339(&ts, rfc_ts, sizeof(rfc_ts));

			if (cfg->host_override[0] != '\0') {
				strncpy(hostbuf, cfg->host_override, sizeof(hostbuf)-1);
				hostbuf[sizeof(hostbuf)-1] = '\0';
			} else {
				if (gethostname(hostbuf, sizeof(hostbuf)) != 0) {
					strncpy(hostbuf, "unknown", sizeof(hostbuf));
					hostbuf[sizeof(hostbuf)-1] = '\0';
				}
			}
			json_escape_string(hostbuf, host_json, sizeof(host_json));

			if (cpu_ptr) {
				char json[1024];
				snprintf(json, sizeof(json),
					"{\"timestamp\":\"%s\",\"host\":%s,\"source\":\"syswatch\",\"event_type\":\"system.metrics.cpu\",\"severity\":\"info\",\"payload\":{\"usage_pct\":%.1f,\"user_pct\":%.1f,\"system_pct\":%.1f,\"idle_pct\":%.1f,\"core_count\":%d}}",
					rfc_ts, host_json, cpu_ptr->usage_pct, cpu_ptr->user_pct, cpu_ptr->system_pct, cpu_ptr->idle_pct, cpu_ptr->core_count);
				queue_enqueue(cfg->event_queue, json, strlen(json));
			}

			if (mem_ptr) {
				char json[1024];
				snprintf(json, sizeof(json),
					"{\"timestamp\":\"%s\",\"host\":%s,\"source\":\"syswatch\",\"event_type\":\"system.metrics.memory\",\"severity\":\"info\",\"payload\":{\"mem_total\":%llu,\"mem_available\":%llu,\"mem_free\":%llu,\"mem_used\":%llu,\"swap_total\":%llu,\"swap_free\":%llu,\"swap_used\":%llu}}",
					rfc_ts, host_json, mem_ptr->mem_total, mem_ptr->mem_available, mem_ptr->mem_free, mem_ptr->mem_used, mem_ptr->swap_total, mem_ptr->swap_free, mem_ptr->swap_used);
				queue_enqueue(cfg->event_queue, json, strlen(json));
			}

			if (disk_ptr) {
				char json[2048];
				int pos = snprintf(json, sizeof(json),
					"{\"timestamp\":\"%s\",\"host\":%s,\"source\":\"syswatch\",\"event_type\":\"system.metrics.disk\",\"severity\":\"info\",\"payload\":{\"disks\":[",
					rfc_ts, host_json);
				for (int i = 0; i < disk_ptr->count && pos < (int)sizeof(json) - 512; i++) {
					if (i > 0) pos += snprintf(json + pos, sizeof(json) - pos, ",");
					pos += snprintf(json + pos, sizeof(json) - pos,
						"{\"name\":\"%s\",\"read_bps\":%.0f,\"write_bps\":%.0f}",
						disk_ptr->items[i].name, disk_ptr->items[i].read_bps, disk_ptr->items[i].write_bps);
				}
				snprintf(json + pos, sizeof(json) - pos, "]}}");
				queue_enqueue(cfg->event_queue, json, strlen(json));
			}

			if (net_ptr) {
				char json[2048];
				int pos = snprintf(json, sizeof(json),
					"{\"timestamp\":\"%s\",\"host\":%s,\"source\":\"syswatch\",\"event_type\":\"system.metrics.network\",\"severity\":\"info\",\"payload\":{\"interfaces\":[",
					rfc_ts, host_json);
				for (int i = 0; i < net_ptr->count && pos < (int)sizeof(json) - 512; i++) {
					if (i > 0) pos += snprintf(json + pos, sizeof(json) - pos, ",");
					pos += snprintf(json + pos, sizeof(json) - pos,
						"{\"name\":\"%s\",\"rx_bps\":%.0f,\"tx_bps\":%.0f}",
						net_ptr->items[i].name, net_ptr->items[i].rx_bps, net_ptr->items[i].tx_bps);
				}
				snprintf(json + pos, sizeof(json) - pos, "]}}");
				queue_enqueue(cfg->event_queue, json, strlen(json));
			}

			/* Emit self-metrics */
			{
				cfg->internal_metrics.buffer_depth = queue_size(cfg->event_queue);
				cfg->internal_metrics.events_dropped_total = queue_dropped_count(cfg->event_queue);

				char json[1024];
				snprintf(json, sizeof(json),
					"{\"timestamp\":\"%s\",\"host\":%s,\"source\":\"syswatch\",\"event_type\":\"syswatch.internal\",\"severity\":\"info\",\"payload\":{\"buffer_depth\":%llu,\"events_dropped_total\":%llu,\"events_emitted_total\":%llu}}",
					rfc_ts, host_json, cfg->internal_metrics.buffer_depth, cfg->internal_metrics.events_dropped_total, cfg->internal_metrics.events_emitted_total);
				queue_enqueue(cfg->event_queue, json, strlen(json));
				cfg->internal_metrics.events_emitted_total += 5;  /* 4 metrics + 1 self-metric */
			}

			/* Evaluate Anomaly Detection for CPU */
			if (cpu_ptr) {
				double mean, stddev;
				if (rolling_stat_check(&cpu_rolling, cpu_ptr->usage_pct, &mean, &stddev)) {
					char json[1024];
					snprintf(json, sizeof(json),
						"{\"timestamp\":\"%s\",\"host\":%s,\"source\":\"syswatch\",\"event_type\":\"syswatch.anomaly\",\"severity\":\"high\",\"payload\":{\"metric\":\"cpu.usage\",\"value\":%.1f,\"expected\":%.1f,\"sigma\":%.1f}}",
						rfc_ts, host_json, cpu_ptr->usage_pct, mean, stddev);
					queue_enqueue(cfg->event_queue, json, strlen(json));
				}
				rolling_stat_add(&cpu_rolling, cpu_ptr->usage_pct);
			}

			/* Evaluate Adaptive Sampling */
			bool is_interesting = false;
			if (cpu_ptr && cpu_ptr->usage_pct > 80.0) is_interesting = true;
			if (mem_ptr && mem_ptr->mem_total > 0 && ((double)mem_ptr->mem_used / (double)mem_ptr->mem_total) > 0.90) is_interesting = true;

			if (is_interesting) {
				high_res_remaining = 30.0;
				if (!high_res_active && cfg->interval_sec > 1) {
					high_res_active = true;
					current_interval = 1;
					char json[1024];
					snprintf(json, sizeof(json),
						"{\"timestamp\":\"%s\",\"host\":%s,\"source\":\"syswatch\",\"event_type\":\"syswatch.lifecycle\",\"severity\":\"warn\",\"payload\":{\"state\":\"high_resolution_mode\",\"reason\":\"load_threshold_exceeded\"}}",
						rfc_ts, host_json);
					queue_enqueue(cfg->event_queue, json, strlen(json));
				}
			} else if (high_res_active) {
				high_res_remaining -= delta_sec;
				if (high_res_remaining <= 0.0) {
					high_res_active = false;
					current_interval = cfg->interval_sec;
					char json[1024];
					snprintf(json, sizeof(json),
						"{\"timestamp\":\"%s\",\"host\":%s,\"source\":\"syswatch\",\"event_type\":\"syswatch.lifecycle\",\"severity\":\"info\",\"payload\":{\"state\":\"normal_resolution_mode\",\"reason\":\"load_normalized\"}}",
						rfc_ts, host_json);
					queue_enqueue(cfg->event_queue, json, strlen(json));
				}
			}
		}

		rows++;
	}

	return NULL;
}

/* Delivery thread: drains queue and sends events to output backends */
static void *delivery_thread(void *arg)
{
	syswatch_config_t *cfg = (syswatch_config_t *)arg;
	if (!cfg || !cfg->event_queue) {
		return NULL;
	}

	event_queue_entry_t batch[1000];
	int batch_count = 0;

	while (!g_stop || queue_size(cfg->event_queue) > 0) {
		/* Dequeue batch */
		if (queue_dequeue_batch(cfg->event_queue, batch, 1000, &batch_count) == 0 && batch_count > 0) {
			for (int i = 0; i < batch_count; i++) {
				output_emit_event(batch[i].json_data);
			}
			output_flush();
		} else {
			/* No events available, sleep briefly to avoid busy-wait */
			usleep(10000);  /* 10ms sleep */
		}
	}

	/* Final flush at shutdown */
	output_flush();

	return NULL;
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
	printf("      --config PATH       YAML config path (use - to read stdin)\n");
	printf("      --validate-config   Parse and validate config then exit\n");
	printf("      --version           Print version and exit\n");
}

int parse_args(int argc, char **argv, syswatch_config_t *cfg)
{
	int opt;
	int long_idx;
	static struct option long_opts[] = {
		{"interval", required_argument, NULL, 'i'},
		{"iterations", required_argument, NULL, 'n'},
		{"csv", no_argument, NULL, 'c'},
		{"config", required_argument, NULL, 2001},
		{"validate-config", no_argument, NULL, 2002},
		{"version", no_argument, NULL, 2003},
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
		case 2001:
			strncpy(cfg->config_path, optarg, sizeof(cfg->config_path) - 1);
			break;
		case 2002:
			cfg->validate_only = true;
			break;
		case 2003:
			printf("syswatch 0.1.0\n");
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

	if (cfg->validate_only && cfg->config_path[0] == '\0' && optind < argc) {
		strncpy(cfg->config_path, argv[optind], sizeof(cfg->config_path) - 1);
		optind++;
	}

	if (optind < argc) {
		fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
		return -1;
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
	int rc = 0;

	init_default_config(&cfg);
	if (parse_args(argc, argv, &cfg) != 0) {
		print_usage(argv[0]);
		return 1;
	}

	if (cfg.config_path[0] == '\0') {
		fprintf(stderr, "--config <path> is required\n");
		print_usage(argv[0]);
		return 1;
	}

	/* Load and validate YAML config */
	{
		char *err = NULL;
		if (load_config_file(cfg.config_path, &cfg, &err) != 0) {
			fprintf(stderr, "config load error: %s\n", err ? err : "unknown");
			return 2;
		}
		if (cfg.validate_only) {
			/* validate-only mode */
			char *verr = NULL;
			int r = validate_config(&cfg, &verr);
			if (r != 0) {
				fprintf(stderr, "config validation failed: %s\n", verr ? verr : "error");
				return 3;
			}
			printf("config OK\n");
			return 0;
		}
	}

	/* Initialize output backend */
	{
		char *oerr = NULL;
		if (output_init(&cfg, &oerr) != 0) {
			fprintf(stderr, "output init failed: %s\n", oerr ? oerr : "unknown");
			return 4;
		}

		/* Instrumentation: report selected output backend */
		int mode = output_get_mode();
		switch (mode) {
		case 0:
			fprintf(stderr, "syswatch: runtime backend=stdout\n");
			break;
		case 1:
			fprintf(stderr, "syswatch: runtime backend=file\n");
			break;
		case 2:
			fprintf(stderr, "syswatch: runtime backend=http_post\n");
			break;
		default:
			fprintf(stderr, "syswatch: runtime backend=unknown(%d)\n", mode);
			break;
		}
	}

	/* Set up signal handlers */
	signal(SIGINT, handle_sigint);
	signal(SIGTERM, handle_sigint);

	/* Store global config for signal handler and threads */
	g_cfg = &cfg;

	/* Spawn collector thread (reads metrics, enqueues events) */
	if (pthread_create(&g_collector_thread, NULL, collector_thread, &cfg) != 0) {
		fprintf(stderr, "failed to create collector thread\n");
		output_shutdown();
		return 5;
	}

	/* Spawn delivery thread (drains queue, sends to outputs) */
	if (pthread_create(&g_delivery_thread, NULL, delivery_thread, &cfg) != 0) {
		fprintf(stderr, "failed to create delivery thread\n");
		g_stop = 1;
		pthread_join(g_collector_thread, NULL);
		output_shutdown();
		return 6;
	}

	/* Main thread: wait for collector to finish */
	pthread_join(g_collector_thread, NULL);

	/* Signal delivery thread to wrap up remaining events and exit */
	g_stop = 1;

	/* Wait for delivery thread to finish (with a reasonable timeout by setting a signal) */
	pthread_join(g_delivery_thread, NULL);

	/* Final shutdown */
	output_shutdown();

	if (cfg.event_queue) {
		queue_destroy(cfg.event_queue);
	}

	return rc;
}
