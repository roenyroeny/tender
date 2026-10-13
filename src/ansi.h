#pragma once

#include "capture.h"

// Universal fallback renderer: draw an RGB frame using 24-bit-colour upper
// half-block characters ('▀'), which works on any modern truecolor terminal.
// One cell shows one horizontal pixel and two vertical pixels (foreground =
// top pixel, background = bottom pixel).
namespace ansi {

void transmit_rgb(int out_fd, const Frame& frame, int top, int left,
                  int cols, int rows, int scr_rows);

}  // namespace ansi
