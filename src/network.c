#include "syswatch.h"

#include <stdio.h>
#include <string.h>

static int find_iface(const net_snapshot_t *snap, const char *name)
{
	int i;
	for (i = 0; i < snap->count; i++) {
		if (strcmp(snap->items[i].name, name) == 0) {
			return i;
		}
	}
	return -1;
}

int net_read_snapshot(net_snapshot_t *out, bool include_loopback)
{
	FILE *fp;
	char line[512];
	int line_no;

	if (!out) {
		return -1;
	}

	memset(out, 0, sizeof(*out));
	fp = fopen("/proc/net/dev", "r");
	if (!fp) {
		return -1;
	}

	line_no = 0;
	while (fgets(line, sizeof(line), fp) && out->count < MAX_INTERFACES) {
		char ifname[32];
		char *name_start;
		unsigned long long rx_packets;
		unsigned long long rx_errs;
		unsigned long long rx_drop;
		unsigned long long rx_fifo;
		unsigned long long rx_frame;
		unsigned long long rx_compressed;
		unsigned long long rx_multicast;
		unsigned long long rx_bytes;
		unsigned long long tx_packets;
		unsigned long long tx_errs;
		unsigned long long tx_drop;
		unsigned long long tx_fifo;
		unsigned long long tx_colls;
		unsigned long long tx_carrier;
		unsigned long long tx_compressed;
		unsigned long long tx_bytes;
		int n;

		line_no++;
		if (line_no <= 2) {
			continue;
		}

		name_start = trim_whitespace(line);
		n = sscanf(name_start,
			"%31[^:]: %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
			ifname,
			&rx_bytes,
			&rx_packets,
			&rx_errs,
			&rx_drop,
			&rx_fifo,
			&rx_frame,
			&rx_compressed,
			&rx_multicast,
			&tx_bytes,
			&tx_packets,
			&tx_errs,
			&tx_drop,
			&tx_fifo,
			&tx_colls,
			&tx_carrier,
			&tx_compressed);
		if (n != 17) {
			continue;
		}

		(void)rx_packets;
		(void)rx_errs;
		(void)rx_drop;
		(void)rx_fifo;
		(void)rx_frame;
		(void)rx_compressed;
		(void)rx_multicast;
		(void)tx_packets;
		(void)tx_errs;
		(void)tx_drop;
		(void)tx_fifo;
		(void)tx_colls;
		(void)tx_carrier;
		(void)tx_compressed;

		if (!include_loopback && strcmp(ifname, "lo") == 0) {
			continue;
		}

		snprintf(out->items[out->count].name, sizeof(out->items[out->count].name), "%s", ifname);
		out->items[out->count].rx_bytes = rx_bytes;
		out->items[out->count].tx_bytes = tx_bytes;
		out->count++;
	}

	fclose(fp);
	return 0;
}

int net_compute_stats(const net_snapshot_t *prev, const net_snapshot_t *curr, int interval_sec, net_stats_t *out)
{
	int i;

	if (!prev || !curr || !out || interval_sec <= 0) {
		return -1;
	}

	memset(out, 0, sizeof(*out));

	for (i = 0; i < curr->count && out->count < MAX_INTERFACES; i++) {
		int idx;
		const net_counter_t *c;
		const net_counter_t *p;
		net_rate_entry_t *dst;
		unsigned long long rx_delta;
		unsigned long long tx_delta;

		c = &curr->items[i];
		idx = find_iface(prev, c->name);
		if (idx < 0) {
			continue;
		}
		p = &prev->items[idx];

		rx_delta = (c->rx_bytes >= p->rx_bytes) ? (c->rx_bytes - p->rx_bytes) : 0;
		tx_delta = (c->tx_bytes >= p->tx_bytes) ? (c->tx_bytes - p->tx_bytes) : 0;

		dst = &out->items[out->count++];
		snprintf(dst->name, sizeof(dst->name), "%s", c->name);
		dst->rx_bps = (double)rx_delta / (double)interval_sec;
		dst->tx_bps = (double)tx_delta / (double)interval_sec;
	}

	return 0;
}
