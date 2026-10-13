#pragma once

#include "capture.h"

// Encode an RGB Frame as a sixel image and write it to `out_fd`, positioned at
// the current cursor location. Unlike the Kitty protocol, sixel has no image
// IDs or placement controls, so the image is simply drawn where the cursor is.
namespace sixel {

void transmit_rgb(int out_fd, const Frame& frame);

// Fill a w x h pixel area with a solid color at the current cursor. Used to
// erase a previous sixel image in terminals that don't clear sixel on text.
void clear(int out_fd, int w, int h, int r, int g, int b);

}  // namespace sixel
