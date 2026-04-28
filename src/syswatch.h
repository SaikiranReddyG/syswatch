#ifndef SYSWATCH_H
#define SYSWATCH_H

#include <stdbool.h>
#include <stddef.h>

#define DEFAULT_REFRESH_INTERVAL 1
#define DEFAULT_ITERATIONS 0
#define DEFAULT_TOP_N 5

#define MAX_INTERFACES 32
#define MAX_DISKS 32
#define MAX_CORES 256
#define MAX_PROCESSES 2048

#define COLW_TIME 8
#define COLW_SMALL 7
#define COLW_MEDIUM 10

typedef enum {
	PROCESS_SORT_CPU = 0,
	PROCESS_SORT_MEM = 1
} process_sort_mode_t;

typedef struct {
	int interval_sec;
	int iterations;
	bool csv_mode;

	bool show_cpu;
	bool show_memory;
	bool show_disk;
	bool show_network;
	bool show_processes;
	bool show_disk_details;
	bool show_network_details;

	bool include_loopback;
	process_sort_mode_t process_sort;
	int top_n;
	char config_path[256];
	bool validate_only;
	/* New config fields from YAML */
	char config_version[16];
	char output_type[16]; /* stdout | file | http_post */
	char output_url[256];
	char output_path[256];
	int output_batch_size;
	int output_batch_interval_seconds;
	int output_retry_max_attempts;
	int output_retry_backoff_seconds;

	char log_level[16];
	char log_destination[16];
	char log_path[256];

	char host_override[128];
} syswatch_config_t;

typedef struct {
	unsigned long long user;
	unsigned long long nice;
	unsigned long long system;
	unsigned long long idle;
	unsigned long long iowait;
	unsigned long long irq;
	unsigned long long softirq;
	unsigned long long steal;
} cpu_times_t;

typedef struct {
	cpu_times_t total;
	int core_count;
	cpu_times_t cores[MAX_CORES];
} cpu_snapshot_t;

typedef struct {
	double usage_pct;
	double user_pct;
	double system_pct;
	double idle_pct;
	int core_count;
	double core_usage_pct[MAX_CORES];
} cpu_stats_t;

typedef struct {
	unsigned long long mem_total;
	unsigned long long mem_available;
	unsigned long long mem_free;
	unsigned long long buffers;
	unsigned long long cached;
	unsigned long long swap_total;
	unsigned long long swap_free;
	unsigned long long mem_used;
	unsigned long long swap_used;
} memory_stats_t;

typedef struct {
	char name[32];
	unsigned long long sectors_read;
	unsigned long long sectors_written;
} disk_counter_t;

typedef struct {
	int count;
	disk_counter_t items[MAX_DISKS];
} disk_snapshot_t;

typedef struct {
	char name[32];
	double read_bps;
	double write_bps;
} disk_rate_entry_t;

typedef struct {
	int count;
	disk_rate_entry_t items[MAX_DISKS];
} disk_stats_t;

typedef struct {
	char name[32];
	unsigned long long rx_bytes;
	unsigned long long tx_bytes;
} net_counter_t;

typedef struct {
	int count;
	net_counter_t items[MAX_INTERFACES];
} net_snapshot_t;

typedef struct {
	char name[32];
	double rx_bps;
	double tx_bps;
} net_rate_entry_t;

typedef struct {
	int count;
	net_rate_entry_t items[MAX_INTERFACES];
} net_stats_t;

typedef struct {
	int pid;
	char comm[64];
	double cpu_pct;
	unsigned long long mem_bytes;
} process_entry_t;

typedef struct {
	int count;
	process_entry_t items[MAX_PROCESSES];
} process_list_t;

void init_default_config(syswatch_config_t *cfg);
int parse_args(int argc, char **argv, syswatch_config_t *cfg);
void print_usage(const char *prog);

int cpu_read_snapshot(cpu_snapshot_t *out);
int cpu_compute_stats(const cpu_snapshot_t *prev, const cpu_snapshot_t *curr, cpu_stats_t *out);

int memory_read_stats(memory_stats_t *out);

int disk_read_snapshot(disk_snapshot_t *out);
int disk_compute_stats(const disk_snapshot_t *prev, const disk_snapshot_t *curr, int interval_sec, disk_stats_t *out);

int net_read_snapshot(net_snapshot_t *out, bool include_loopback);
int net_compute_stats(const net_snapshot_t *prev, const net_snapshot_t *curr, int interval_sec, net_stats_t *out);

int process_read_list(process_list_t *out, process_sort_mode_t sort_mode, int limit);

void display_print_header(const syswatch_config_t *cfg);
void display_print_row(const syswatch_config_t *cfg,
	const cpu_stats_t *cpu,
	const memory_stats_t *mem,
	const disk_stats_t *disk,
	const net_stats_t *net,
	const process_list_t *plist);

int read_first_line(const char *path, char *buf, size_t size);
char *trim_whitespace(char *s);
int parse_ull(const char *s, unsigned long long *out);
double clamp_double(double val, double min, double max);
void format_bytes(double bytes, char *buf, size_t len);
void format_timestamp(char *buf, size_t len);

/* Config loader (simple YAML subset) */
int load_config_file(const char *path, syswatch_config_t *cfg, char **err);
int validate_config(const syswatch_config_t *cfg, char **err);

/* Output abstraction */
int output_init(const syswatch_config_t *cfg, char **err);
int output_emit_event(const char *json_line);
int output_flush(void);
void output_shutdown(void);
int output_get_mode(void);
void json_escape_string(const char *input, char *output, size_t output_size);

#endif
