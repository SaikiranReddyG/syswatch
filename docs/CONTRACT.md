# Integration Contract (syswatch / codex tools)

This document defines the minimal contract that codex tools (including `syswatch`) must honour to be integrable by the platform.

## Event schema
Every event emitted by a tool is a JSON object with at least these fields:

| Field | Type | Required | Description |
|---|---:|---:|---|
| `schema_version` | string | yes | Semver of the event schema, e.g., "1.0" |
| `timestamp` | string (RFC 3339) | yes | When the event was generated, with timezone |
| `source` | string | yes | Tool that emitted it, e.g., "syswatch" |
| `source_version` | string | yes | Tool version, e.g., "0.1.0" |
| `host` | string | yes | Hostname of the machine that generated it |
| `event_type` | string | yes | Category, e.g., "system.metrics" |
| `severity` | string | yes | One of `info`, `low`, `medium`, `high`, `critical` |
| `payload` | object | yes | Tool-specific data |

Example CPU event:

```json
{
  "schema_version": "1.0",
  "timestamp": "2026-04-28T14:23:01.234+02:00",
  "source": "syswatch",
  "source_version": "0.1.0",
  "host": "myhost",
  "event_type": "system.metrics.cpu",
  "severity": "info",
  "payload": { "user_pct": 12.4, "system_pct": 3.1, "idle_pct": 81.2 }
}
```

Events must be line-delimited JSON when streamed to `stdout` or appended to files.

## Config conventions
- YAML format
- File path supplied via `--config <path>`
- `--config -` reads from stdin
- `--validate-config` parses and exits without running, returning non-zero on invalid

See `syswatch.example.yaml` for the initial schema.

## Output destinations
Tools must support at minimum:
- `stdout` — line-delimited JSON
- `http_post` — POST batches as JSON array to configured URL, with retry/backoff
- `file` — append-only line-delimited JSON file

## Lifecycle
- Run in foreground (do not daemonize)
- Logs to `stderr` by default, optionally to a configured file
- Configurable log level: `debug`, `info`, `warn`, `error`
- `SIGTERM` / `SIGINT` trigger graceful shutdown: stop sampling, flush buffers, exit 0 within 5s
- `SIGHUP` reserved for future reloads

## Versioning
- `--version` outputs `<name> <semver> (<git-commit-short>)` and exits 0
- Schema version in events is independent of tool version
