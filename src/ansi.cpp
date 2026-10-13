#include "ansi.h"

#include <cerrno>
#include <cstdint>
#include <string>
#include <unistd.h>

namespace ansi {

namespace {

void write_all(int fd, const char* data, size_t len) {
    while (len > 0) {
        ssize_t n = ::write(fd, data, len);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        data += n;
        len -= static_cast<size_t>(n);
    }
}

void append_sgr(std::string& out, const uint8_t* px) {
    out += "\x1b[38;2;";
    out += std::to_string(px[0]);
    out += ';';
    out += std::to_string(px[1]);
    out += ';';
    out += std::to_string(px[2]);
    out += 'm';
}

}  // namespace

void transmit_rgb(int out_fd, const Frame& frame, int top, int left,
                  int cols, int rows, int scr_rows) {
    const int sw = frame.width;
    const int sh = frame.height;
    if (sw <= 0 || sh <= 0 || cols <= 0 || rows <= 0) return;
    if (frame.rgb.size() < static_cast<size_t>(sw) * sh * 3) return;

    const int out_h = rows * 2;  // source rows sampled per cell column
    std::string out;
    out.reserve(static_cast<size_t>(cols) * 24);

    for (int r = 0; r < rows; ++r) {
        int row = top + r;
        if (row < 1 || (scr_rows > 0 && row > scr_rows)) continue;
        out += "\x1b[";
        out += std::to_string(row);
        out += ';';
        out += std::to_string(left);
        out += 'H';
        int yt = static_cast<int>(static_cast<long>(r * 2) * sh / out_h);
        int yb = static_cast<int>(static_cast<long>(r * 2 + 1) * sh / out_h);
        if (yt >= sh) yt = sh - 1;
        if (yb >= sh) yb = sh - 1;
        const uint8_t* rowt = &frame.rgb[static_cast<size_t>(yt) * sw * 3];
        const uint8_t* rowb = &frame.rgb[static_cast<size_t>(yb) * sw * 3];
        for (int c = 0; c < cols; ++c) {
            int sx = static_cast<int>(static_cast<long>(c) * sw / cols);
            if (sx >= sw) sx = sw - 1;
            append_sgr(out, rowt + static_cast<size_t>(sx) * 3);
            out += "\x1b[48;2;";
            out += std::to_string(rowb[sx * 3 + 0]);
            out += ';';
            out += std::to_string(rowb[sx * 3 + 1]);
            out += ';';
            out += std::to_string(rowb[sx * 3 + 2]);
            out += 'm';
            out += "\xe2\x96\x80";  // U+2580 upper half block
        }
        out += "\x1b[0m";
        if (out.size() > (1u << 16)) {
            write_all(out_fd, out.data(), out.size());
            out.clear();
        }
    }
    if (!out.empty()) write_all(out_fd, out.data(), out.size());
}

}  // namespace ansi
