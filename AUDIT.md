# syswatch AUDIT

## File-by-file classification
- `src/main.c` — Refactor (needs config loading, signal handling, switch to JSON event output via new output destinations, remove old command-line arg parsing)
- `src/display.c` — Rewrite (currently writes human-readable CLI tables and CSV, MUST be rewritten to dispatch JSON events to stdout, file, or http_post)
- `src/cpu.c` — Keep (clean `/proc/stat` parsing)
- `src/disk.c` — Keep (clean `/proc/diskstats` parsing)
- `src/memory.c` — Keep (clean `/proc/meminfo` parsing)
- `src/network.c` — Keep (clean `/proc/net/dev` parsing)
- `src/process.c` — Keep (clean `/proc/*/stat` parsing)
- `src/syswatch.h` — Refactor (remove old display logic config structs, add new yaml config structs and output abstractions)
- `src/utils.c` — Keep (helper functions for text parsing)
- `Makefile` — Refactor (add libcyaml and libcurl dependencies)
- `README.md` — Rewrite (currently assumes platform context, needs to reflect standalone usage)

## Tangle points
- Hardcoded command line arguments in `main.c` with no proper configuration file mechanism.
- `display.c` acts as the single output mechanism, deeply coupled with stdout.
- No `gethostname()` logic currently, host needs to be passed via config or fetched natively.

## What works today
- Accurate monitoring of CPU (`/proc/stat`), memory (`/proc/meminfo`), disk (`/proc/diskstats`), network (`/proc/net/dev`) and processes.
- Clear computing of deltas between intervals.

## Questions to answer
- Which YAML parsing and JSON formatting libraries should we use? A simple solution is `libcyaml` for YAML and something like `cJSON` or `json-c` for JSON, or manual string construction for simplicity? In C, manual string construction for simple JSON arrays/objects is easy but `libcurl` is needed for HTTP. The project requires HTTP POST with retries.

## Actions taken (initial)
- Updated `README.md` with quickstart and config guidance.
- Added `syswatch.example.yaml` (example config schema).
- Added `examples/systemd/syswatch.service` (sample unit file).
- Added `test/smoke.sh` (basic smoke testing script).
- Added `LICENSE` (MIT).

