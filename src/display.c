#include "syswatch.h"

#include <stdio.h>

#define ANSI_RESET "\033[0m"
#define ANSI_RED "\033[31m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_GREEN "\033[32m"

static const char *cpu_color(double usage)
{
	if (usage >= 85.0) {
		return ANSI_RED;
	}
	if (usage >= 60.0) {
		return ANSI_YELLOW;
	}
	return ANSI_GREEN;
}

static void sum_disk_rates(const disk_stats_t *disk, double *read_bps, double *write_bps)
{
	int i;
	*read_bps = 0.0;
	*write_bps = 0.0;
	if (!disk) {
		return;
	}
	for (i = 0; i < disk->count; i++) {
		*read_bps += disk->items[i].read_bps;
		*write_bps += disk->items[i].write_bps;
	}
}

static void sum_net_rates(const net_stats_t *net, double *rx_bps, double *tx_bps)
{
	int i;
	*rx_bps = 0.0;
	*tx_bps = 0.0;
	if (!net) {
		return;
	}
	for (i = 0; i < net->count; i++) {
		*rx_bps += net->items[i].rx_bps;
		*tx_bps += net->items[i].tx_bps;
	}
}

void display_print_header(const syswatch_config_t *cfg)
{
	if (!cfg) {
		return;
	}

	if (cfg->csv_mode) {
		printf("timestamp");
		if (cfg->show_cpu) {
			printf(",cpu_usage_pct,cpu_user_pct,cpu_system_pct,cpu_idle_pct");
		}
		if (cfg->show_memory) {
			printf(",mem_used_bytes,mem_available_bytes,swap_used_bytes");
		}
		if (cfg->show_disk) {
			printf(",disk_read_bps,disk_write_bps");
		}
		if (cfg->show_network) {
			printf(",net_rx_bps,net_tx_bps");
		}
		if (cfg->show_processes) {
			printf(",top_pid,top_comm,top_cpu_pct,top_mem_bytes");
		}
		printf("\n");
		return;
	}

	printf("%-8s", "time");
	if (cfg->show_cpu) {
		printf(" %6s %6s %6s %6s", "cpu%", "usr%", "sys%", "idl%");
	}
	if (cfg->show_memory) {
		printf(" %10s %10s %10s", "mem_used", "mem_avail", "swap_used");
	}
	if (cfg->show_disk) {
		printf(" %10s %10s", "disk_rd/s", "disk_wr/s");
	}
	if (cfg->show_network) {
		printf(" %10s %10s", "net_rx/s", "net_tx/s");
	}
	if (cfg->show_processes) {
		printf("  top_processes");
	}
	printf("\n");
}

void display_print_row(const syswatch_config_t *cfg,
	const cpu_stats_t *cpu,
	const memory_stats_t *mem,
	const disk_stats_t *disk,
	const net_stats_t *net,
	const process_list_t *plist)
{
	char ts[16];
	double disk_read_bps;
	double disk_write_bps;
	double net_rx_bps;
	double net_tx_bps;

	if (!cfg) {
		return;
	}

	format_timestamp(ts, sizeof(ts));
	sum_disk_rates(disk, &disk_read_bps, &disk_write_bps);
	sum_net_rates(net, &net_rx_bps, &net_tx_bps);

	if (cfg->csv_mode) {
		printf("%s", ts);
		if (cfg->show_cpu) {
			if (cpu) {
				printf(",%.2f,%.2f,%.2f,%.2f", cpu->usage_pct, cpu->user_pct, cpu->system_pct, cpu->idle_pct);
			} else {
				printf(",0,0,0,0");
			}
		}
		if (cfg->show_memory) {
			if (mem) {
				printf(",%llu,%llu,%llu", mem->mem_used, mem->mem_available, mem->swap_used);
			} else {
				printf(",0,0,0");
			}
		}
		if (cfg->show_disk) {
			printf(",%.2f,%.2f", disk_read_bps, disk_write_bps);
		}
		if (cfg->show_network) {
			printf(",%.2f,%.2f", net_rx_bps, net_tx_bps);
		}
		if (cfg->show_processes) {
			if (plist && plist->count > 0) {
				const process_entry_t *p = &plist->items[0];
				printf(",%d,%s,%.2f,%llu", p->pid, p->comm, p->cpu_pct, p->mem_bytes);
			} else {
				printf(",0,,0,0");
			}
		}
		printf("\n");
		return;
	}

	printf("%-8s", ts);
	if (cfg->show_cpu) {
		if (cpu) {
			printf(" %s%6.1f%s %6.1f %6.1f %6.1f", cpu_color(cpu->usage_pct), cpu->usage_pct, ANSI_RESET,
				cpu->user_pct, cpu->system_pct, cpu->idle_pct);
		} else {
			printf(" %6s %6s %6s %6s", "N/A", "N/A", "N/A", "N/A");
		}
	}
	if (cfg->show_memory) {
		char used[16];
		char avail[16];
		char swap[16];

		if (mem) {
			format_bytes((double)mem->mem_used, used, sizeof(used));
			format_bytes((double)mem->mem_available, avail, sizeof(avail));
			format_bytes((double)mem->swap_used, swap, sizeof(swap));
			printf(" %10s %10s %10s", used, avail, swap);
		} else {
			printf(" %10s %10s %10s", "N/A", "N/A", "N/A");
		}
	}
	if (cfg->show_disk) {
		char rd[16];
		char wr[16];

		format_bytes(disk_read_bps, rd, sizeof(rd));
		format_bytes(disk_write_bps, wr, sizeof(wr));
		printf(" %10s %10s", rd, wr);
	}
	if (cfg->show_network) {
		char rx[16];
		char tx[16];

		format_bytes(net_rx_bps, rx, sizeof(rx));
		format_bytes(net_tx_bps, tx, sizeof(tx));
		printf(" %10s %10s", rx, tx);
	}

	if (cfg->show_processes) {
		int i;
		int shown;

		printf("  ");
		shown = 0;
		if (plist) {
			for (i = 0; i < plist->count && shown < cfg->top_n; i++) {
				char m[16];
				format_bytes((double)plist->items[i].mem_bytes, m, sizeof(m));
				printf("%d:%s(%.1f%%,%s)", plist->items[i].pid, plist->items[i].comm, plist->items[i].cpu_pct, m);
				shown++;
				if (i + 1 < plist->count && shown < cfg->top_n) {
					printf(" ");
				}
			}
		}
		if (shown == 0) {
			printf("none");
		}
	}

	printf("\n");
}
