#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

MATRIX_DELAY="${MATRIX_DELAY:-180}" \
MATRIX_MODE=lofi \
MATRIX_R="${MATRIX_R:-180}" \
MATRIX_G="${MATRIX_G:-90}" \
MATRIX_B="${MATRIX_B:-255}" \
  "${PROJECT_ROOT}/examples/custom_animation.sh"
