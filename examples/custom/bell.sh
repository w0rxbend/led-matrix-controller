#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

MATRIX_DELAY="${MATRIX_DELAY:-90}" \
MATRIX_MODE=bell \
MATRIX_R="${MATRIX_R:-255}" \
MATRIX_G="${MATRIX_G:-190}" \
MATRIX_B="${MATRIX_B:-40}" \
  "${PROJECT_ROOT}/examples/custom_animation.sh"
