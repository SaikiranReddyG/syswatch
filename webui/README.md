# SysWatch Web UI

A lightweight local dashboard for SysWatch with zero external dependencies.

## What this provides

- `server.py`: receives SysWatch events at `POST /events` and serves:
  - `GET /` dashboard UI
  - `GET /api/health`
  - `GET /api/latest`
  - `GET /api/history`
- `index.html`: live dashboard that refreshes every second.

## Run

From repository root:

```bash
python3 webui/server.py --host 127.0.0.1 --port 8780
```

Open:

```text
http://127.0.0.1:8780
```

## Point SysWatch to Web UI

Use the included ready-to-run config:

```bash
./syswatch --config webui/syswatch-webui.yaml -n 0
```

If the UI is still empty:
- Confirm health and ingest counters:

```bash
curl -s http://127.0.0.1:8780/api/health | jq
```

- You should see `received_events` increasing.
- Use the dashboard button `Inject Demo Events` to validate rendering immediately.

## Quick API checks

```bash
curl -s http://127.0.0.1:8780/api/health | jq
curl -s http://127.0.0.1:8780/api/latest | jq
curl -s "http://127.0.0.1:8780/api/history?limit=10" | jq
```
