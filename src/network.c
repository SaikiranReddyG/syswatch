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
		char *cursor;
		char *token;
		char *name;
		unsigned long long rx_bytes;
		unsigned long long tx_bytes;
		int field;

		line_no++;
		if (line_no <= 2) {
			continue;
		}

		cursor = trim_whitespace(line);
		if (!cursor || *cursor == '\0') {
			continue;
		}

		field = 0;
		name = NULL;
		rx_bytes = 0;
		tx_bytes = 0;
		for (token = strtok(cursor, " \t"); token; token = strtok(NULL, " \t")) {
			field++;
			if (field == 1) {
				char *colon = strchr(token, ':');
				if (!colon) {
					break;
				}
				*colon = '\0';
				name = token;
			} else if (field == 2) {
				if (parse_ull(token, &rx_bytes) != 0) {
					break;
				}
			} else if (field == 10) {
				if (parse_ull(token, &tx_bytes) != 0) {
					break;
				}
				break;
			}
		}

		if (field < 10 || !name) {
			continue;
		}

		if (!include_loopback && strcmp(name, "lo") == 0) {
			continue;
		}

		snprintf(out->items[out->count].name, sizeof(out->items[out->count].name), "%s", name);
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
