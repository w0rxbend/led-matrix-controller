#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

MATRIX_HOST="${MATRIX_HOST:-192.168.1.127}"
MATRIX_PORT="${MATRIX_PORT:-7777}"
MATRIX_DELAY="${MATRIX_DELAY:-90}"
MATRIX_R="${MATRIX_R:-255}"
MATRIX_G="${MATRIX_G:-0}"
MATRIX_B="${MATRIX_B:-0}"
MATRIX_ROTATION="${MATRIX_ROTATION:--90}"
MATRIX_LAYOUT="${MATRIX_LAYOUT:-h-tl}"

python "$PROJECT_ROOT/examples/heart_pulse.py" \
  --host "$MATRIX_HOST" \
  --port "$MATRIX_PORT" \
  --delay "$MATRIX_DELAY" \
  --r "$MATRIX_R" \
  --g "$MATRIX_G" \
  --b "$MATRIX_B" \
  --rotation "$MATRIX_ROTATION" \
  --layout "$MATRIX_LAYOUT"
