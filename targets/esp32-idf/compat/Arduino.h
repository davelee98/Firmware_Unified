/* Arduino.h -- forwarder so the imported sources compile unmodified.
 *
 * The imported files write `#include <Arduino.h>`; phase A imported them byte-identical and
 * phase B's job is to make them build, not to rewrite their includes. This one-line forwarder
 * is what lets both be true.
 *
 * It is part of the shim and dies with it. compat/ratchet.sh counts users of EITHER header,
 * so this file does not launder shim usage past the ratchet -- see ratchet.sh for why that
 * matters.
 */
#pragma once
#include "arduino_compat.h"
