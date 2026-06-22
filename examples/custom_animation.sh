#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

MATRIX_HOST="${MATRIX_HOST:-192.168.1.127}"
MATRIX_PORT="${MATRIX_PORT:-7777}"
MATRIX_DELAY="${MATRIX_DELAY:-120}"

python "$PROJECT_ROOT/examples/custom_animation.py" \
  --host "$MATRIX_HOST" \
  --port "$MATRIX_PORT" \
  --delay "$MATRIX_DELAY"
