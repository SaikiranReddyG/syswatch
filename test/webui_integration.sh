#!/usr/bin/env bash
set -euo pipefail

ROOT=$(dirname "$0")/..
ROOT=$(cd "$ROOT" && pwd)
cd "$ROOT"

BIN=./syswatch
WEBUI_LOG=/tmp/syswatch_webui.log
WEBUI_PORT=8780
WEBUI_HOST=127.0.0.1

# Build
make >/dev/null

# Start webui
python3 webui/server.py --host ${WEBUI_HOST} --port ${WEBUI_PORT} &> ${WEBUI_LOG} &
WEBUI_PID=$!
sleep 0.5

trap 'echo "Stopping webui..."; kill ${WEBUI_PID} 2>/dev/null || true' EXIT

# Create temp config
TMPCFG=$(mktemp /tmp/syswatch_test.XXXX.yaml)
cat > ${TMPCFG} <<EOF
config_version: "1.0"
poll_interval_seconds: 1
collect:
  cpu: true
  memory: true
  disk: false
  network: false
output:
  type: http_post
  url: "http://${WEBUI_HOST}:${WEBUI_PORT}/events"
  batch_size: 2
  batch_interval_seconds: 1
log:
  level: info
host_override: "test-host"
EOF

# Run syswatch for a few iterations
${BIN} --config ${TMPCFG} --iterations 3 >/tmp/syswatch_run.log 2>&1 || true

# Allow webui to process
sleep 1

# Query webui health
HEALTH=$(curl -sSL http://${WEBUI_HOST}:${WEBUI_PORT}/api/health || true)
echo "webui health: ${HEALTH}"

# Basic check: ensure received_events > 0
RECEIVED=$(echo "$HEALTH" | python3 -c 'import sys, json; d=json.load(sys.stdin); print(d.get("received_events",0))')
if [ "$RECEIVED" -gt 0 ]; then
  echo "Integration test: OK (received_events=${RECEIVED})"
  exit 0
else
  echo "Integration test: FAILED (received_events=${RECEIVED})"
  exit 2
fi
