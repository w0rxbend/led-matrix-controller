#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

MATRIX_HOST="${MATRIX_HOST:-192.168.1.127}"
MATRIX_PORT="${MATRIX_PORT:-7777}"

run_preset() {
  local id="$1"
  local name="$2"
  local default_interval="$3"
  local default_r="$4"
  local default_g="$5"
  local default_b="$6"

  local interval="${MATRIX_INTERVAL:-$default_interval}"
  local red="${MATRIX_R:-$default_r}"
  local green="${MATRIX_G:-$default_g}"
  local blue="${MATRIX_B:-$default_b}"

  echo "preset=$name id=$id host=$MATRIX_HOST interval=${interval}ms rgb=$red,$green,$blue"
  python "$PROJECT_ROOT/tools/matrix_client.py" \
    --host "$MATRIX_HOST" \
    --port "$MATRIX_PORT" \
    preset "$id" \
    --interval "$interval" \
    --r "$red" \
    --g "$green" \
    --b "$blue"
}
