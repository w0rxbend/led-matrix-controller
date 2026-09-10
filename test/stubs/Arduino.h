#pragma once

// Minimal Arduino surface for host builds.
//
// The native test environment compiles the protocol and layout units, which need
// nothing from Arduino but fixed-width integer types. Keeping this stub tiny is
// the point: if a unit under test starts needing real Arduino behaviour, it is no
// longer a pure unit and belongs behind a seam instead of in this stub.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
