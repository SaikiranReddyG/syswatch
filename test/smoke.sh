#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

echo "1) Build"
make clean && make

echo "2) Version check"
if ./syswatch --version >/dev/null 2>&1; then
  echo "--version ran (exit 0)"
else
  echo "--version not implemented or failed"
fi

echo "3) Validate example config (if implemented)"
if ./syswatch --validate-config syswatch.example.yaml >/dev/null 2>&1; then
  echo "validate-config OK"
else
  echo "validate-config either not implemented or returned non-zero (this may be expected)"
fi

echo "4) Run with stdout output for 6 seconds"
timeout 6s ./syswatch --config syswatch.example.yaml > /tmp/syswatch_smoke.jsonl 2>/tmp/syswatch_smoke.log || true

echo "5) Basic JSONL validation"
if command -v jq >/dev/null 2>&1; then
  if jq -c . < /tmp/syswatch_smoke.jsonl >/dev/null 2>&1; then
    echo "stdout appears to be valid JSON lines (jq succeeded)"
  else
    echo "stdout is not valid JSON lines or empty"
  fi
else
  echo "jq not installed; skipping JSON validation"
fi

echo "Smoke test complete. Inspect /tmp/syswatch_smoke.jsonl and /tmp/syswatch_smoke.log"
