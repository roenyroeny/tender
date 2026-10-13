#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "capture.h"

namespace kitty {

std::vector<uint8_t> zlib_compress(const uint8_t* data, size_t len);

std::string base64_encode(const uint8_t* data, size_t len);

// Transmit and display an RGB frame. If cols > 0 the frame is scaled by the
// terminal to that many columns (rows derived from the aspect ratio); if cols
// is 0 the image is displayed at its own pixel size (1:1). off_x/off_y shift
// the image by that many pixels within the first cell (used to centre it).
// src_x/src_y/src_w/src_h crop the displayed source area (used to clip a window
// against the edge of the terminal); 0 means "from the edge" / "full size".
void transmit_rgb(int out_fd, const Frame& frame, uint32_t image_id, int cols,
                  int off_x = 0, int off_y = 0,
                  int src_x = 0, int src_y = 0, int src_w = 0, int src_h = 0);

void delete_image(int out_fd, uint32_t image_id);

void delete_all(int out_fd);

}  // namespace kitty
