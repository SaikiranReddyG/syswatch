# Changelog

## 0.1.1 - 2026-05-08

- Standardized emitted event namespaces to `system.metrics.*` and lifecycle/internal events.
- Added `SYSWATCH_VERSION` constant and propagated to demo web UI and docs.
- Split runtime state out of `syswatch_config_t` to a dedicated runtime context to avoid mixing user config and runtime-only fields.
- Removed speculative anomaly detection and adaptive sampling features.
- Slimmed `/proc` parsers to only extract fields actually used by the agent.
- Replaced local `xstrdup` helper with standard `strdup` in `config.c`.
- Removed legacy display renderer (`src/display.c`) and excluded it from the build.
- Improved portable RFC3339 timestamp formatting.
- Minor build and test improvements; added a simple web UI integration smoke test.
