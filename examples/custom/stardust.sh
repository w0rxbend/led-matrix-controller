#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

MATRIX_DELAY="${MATRIX_DELAY:-100}" \
MATRIX_MODE=stardust \
MATRIX_R="${MATRIX_R:-90}" \
MATRIX_G="${MATRIX_G:-150}" \
MATRIX_B="${MATRIX_B:-255}" \
  "${PROJECT_ROOT}/examples/custom_animation.sh"
