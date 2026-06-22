#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

MATRIX_DELAY="${MATRIX_DELAY:-120}" \
MATRIX_MODE=scala_logo \
MATRIX_R="${MATRIX_R:-220}" \
MATRIX_G="${MATRIX_G:-20}" \
MATRIX_B="${MATRIX_B:-30}" \
  "${PROJECT_ROOT}/examples/custom_animation.sh"
