/* Definitions for the shim's one global. Header-only would need C++17 inline variables and
 * the source builds as C++11/14 under Arduino today; keeping a .cpp avoids depending on the
 * standard level while the port is in flux. Dies with the shim. */
#include "arduino_compat.h"

SerialCompat Serial;
