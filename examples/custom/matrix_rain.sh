#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

MATRIX_DELAY="${MATRIX_DELAY:-90}" \
MATRIX_MODE=matrix_rain \
MATRIX_R="${MATRIX_R:-40}" \
MATRIX_G="${MATRIX_G:-255}" \
MATRIX_B="${MATRIX_B:-80}" \
  "${PROJECT_ROOT}/examples/custom_animation.sh"
