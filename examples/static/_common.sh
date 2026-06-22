#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

MATRIX_HOST="${MATRIX_HOST:-192.168.1.127}"
MATRIX_PORT="${MATRIX_PORT:-7777}"

set_static_color() {
  local name="$1"
  local default_r="$2"
  local default_g="$3"
  local default_b="$4"

  local red="${MATRIX_R:-$default_r}"
  local green="${MATRIX_G:-$default_g}"
  local blue="${MATRIX_B:-$default_b}"

  echo "static=$name host=$MATRIX_HOST rgb=$red,$green,$blue"
  python "$PROJECT_ROOT/tools/matrix_client.py" \
    --host "$MATRIX_HOST" \
    --port "$MATRIX_PORT" \
    static "$red" "$green" "$blue"
}
