# syswatch

`syswatch` is a lightweight Linux system monitor written in pure C.
It reads `/proc` directly and prints one row per sampling tick.

Features:
- CPU usage from `/proc/stat` using delta math between samples.
- Memory and swap from `/proc/meminfo` (`used = total - available`).
- Disk throughput from `/proc/diskstats` (sectors delta -> bytes/sec).
- Network throughput from `/proc/net/dev` (rx/tx bytes delta -> bytes/sec).
- Optional top process summary from `/proc/[pid]/stat` and `/proc/[pid]/status`.
- Human-readable table mode and CSV mode for piping to other tools.

## Build

```bash
make
```

Debug build with sanitizers:

```bash
make debug
```

Clean artifacts:

```bash
make clean
```

## Usage

```bash
./syswatch [options]
```

Options:
- `-i, --interval SEC` refresh interval in seconds (default: `1`)
- `-n, --iterations N` number of rows to print (`0` means run forever)
- `-c, --csv` CSV mode (no ANSI colors)
- `--no-cpu` hide CPU columns
- `--no-memory` hide memory columns
- `--no-disk` hide disk columns
- `--no-network` hide network columns
- `--include-lo` include loopback interface (`lo`) in network totals
- `-p, --processes` show top process summary column
- `-t, --top N` number of top processes to include in human mode (default: `5`)
- `-s, --proc-sort MODE` process sorting: `cpu` or `mem`
- `-h, --help` show help

## Examples

Human-readable output:

```bash
./syswatch
```

Run for 10 rows at 2-second interval:

```bash
./syswatch -i 2 -n 10
```

CSV mode to file:

```bash
./syswatch --csv -n 5 > out.csv
```

CSV mode piped to another command:

```bash
./syswatch --csv | tee live.csv
```

Include top processes sorted by memory:

```bash
./syswatch -p -s mem -t 3
```

## Output Notes

- CPU/network/disk values are rates, so the tool captures a baseline and prints the first row after one interval.
- Memory usage uses `MemAvailable`, matching modern Linux memory interpretation.
- Disk and network rows are aggregate totals in main columns; per-device/interface values are still parsed internally.
- Process CPU percentage is based on process CPU time relative to system uptime (good for ranking, not an instantaneous per-tick profiler).

## Progress Log

- [x] Project skeleton (`Makefile`, `src/`, module split).
- [x] Shared contracts and configuration (`src/syswatch.h`).
- [x] Collector implementations for CPU, memory, disk, and network.
- [x] Optional process collector and sorting.
- [x] Dual output renderer (human + CSV).
- [x] Main loop, argument parsing, and Ctrl+C handling.
- [ ] Additional tests/benchmarks for very large process counts and unusual device naming environments.

## Limitations

- Linux-only (`/proc` required).
- No ncurses/TUI mode in this implementation; output is line-oriented and pipe-friendly.
- Some `/proc/[pid]` entries can become unreadable between scan and parse as processes exit; those are skipped.
