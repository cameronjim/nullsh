// One line rendering of a decoded Packet. Pure formatting, no I/O.

#pragma once

#include "../util/str.h"
#include "decode.h"

// Clears out, then writes one line with no trailing newline.
void packet_format(const Packet *p, Str *out);
