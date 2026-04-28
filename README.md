# syswatch

syswatch is a lightweight Linux system metrics collector that reads `/proc` and emits structured JSON events.

**Goals**
- Build from source with `make` and run as `./syswatch --config <path>`
- Emit line-delimited JSON events (one per line) to `stdout`, `file`, or `http_post` according to config
- Run in foreground and exit cleanly on `SIGTERM`/`SIGINT`

**Quickstart**
1. Build:

```bash
make
```

2. Run with the example config:

```bash
./syswatch --config syswatch.example.yaml
```

3. Stop with Ctrl+C or `kill -TERM <pid>`.

**Configuration**
- See `syswatch.example.yaml` in the repository root for the v1.0 config schema and sensible defaults.
- Important flags supported by the runtime:
	- `--config <path>`: path to YAML config file (use `-` to read from stdin)
	- `--validate-config`: parse and validate the config and exit
	- `--version`: print the build version and exit

**Output destinations**
- `stdout`: line-delimited JSON, one event per line
- `file`: append-only JSONL file
- `http_post`: POST batches of events as a JSON array to a configured URL

**Smoke test**
Run the included smoke script (requires a working build):

```bash
bash test/smoke.sh
```

**Further reading**
- Example config: `syswatch.example.yaml`
- Example systemd unit: `examples/systemd/syswatch.service`

If you need the previous human/CSV display modes, those are preserved in `src/display.c` for reference but will be refactored to the new output abstraction.
