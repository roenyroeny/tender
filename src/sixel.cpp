#include "sixel.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <unistd.h>

namespace sixel {

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

// Append an RLE-compressed run of a sixel value to `out`.
void emit_run(std::string& out, int value, int count) {
    if (count <= 0) return;
    char c = static_cast<char>('?' + value);
    if (count > 3) {
        out += '!';
        out += std::to_string(count);
        out += c;
    } else {
        out.append(static_cast<size_t>(count), c);
    }
}

}  // namespace

void clear(int out_fd, int w, int h, int r, int g, int b) {
    if (w <= 0 || h <= 0) return;
    int R = r * 100 / 255, G = g * 100 / 255, B = b * 100 / 255;
    std::string out;
    out += "\x1bPq\"1;1;";
    out += std::to_string(w);
    out += ';';
    out += std::to_string(h);
    out += "#1;2;";
    out += std::to_string(R);
    out += ';';
    out += std::to_string(G);
    out += ';';
    out += std::to_string(B);
    for (int y = 0; y < h; y += 6) {
        int band = std::min(6, h - y);
        int val = (1 << band) - 1;
        char c = static_cast<char>('?' + val);
        out += "#1";
        if (w > 3) {
            out += '!';
            out += std::to_string(w);
            out += c;
        } else {
            out.append(static_cast<size_t>(w), c);
        }
        out += '-';
    }
    out += "\x1b\\";
    write_all(out_fd, out.data(), out.size());
}

void transmit_rgb(int out_fd, const Frame& frame) {
    const int w = frame.width;
    const int h = frame.height;
    if (w <= 0 || h <= 0) return;
    if (frame.rgb.size() < static_cast<size_t>(w) * h * 3) return;

    std::string out;
    out.reserve(static_cast<size_t>(w) * 4 + 64);
    out += "\x1bPq";  // DCS, sixel data
    // Raster attributes: 1:1 pixel aspect and the image extent. Without this
    // many terminals assume a non-square (e.g. 2:1) pixel aspect and render the
    // image stretched.
    out += '"';
    out += "1;1;";
    out += std::to_string(w);
    out += ';';
    out += std::to_string(h);

    bool defined[256] = {false};

    for (int y0 = 0; y0 < h; y0 += 6) {
        const int band = std::min(6, h - y0);

        // Palette index (3-3-2 bits) -> per-column 6-bit mask for this band.
        std::unordered_map<int, std::vector<uint8_t>> masks;
        for (int x = 0; x < w; ++x) {
            for (int by = 0; by < band; ++by) {
                const uint8_t* p = &frame.rgb[(static_cast<size_t>(y0 + by) * w + x) * 3];
                int idx = ((p[0] >> 5) << 5) | ((p[1] >> 5) << 2) | (p[2] >> 6);
                auto it = masks.find(idx);
                if (it == masks.end()) {
                    it = masks.emplace(idx, std::vector<uint8_t>(w, 0)).first;
                }
                it->second[x] |= static_cast<uint8_t>(1u << by);
            }
        }

        for (auto& kv : masks) {
            const int idx = kv.first;
            const std::vector<uint8_t>& v = kv.second;

            out += '#';
            out += std::to_string(idx);
            if (!defined[idx]) {
                defined[idx] = true;
                int r3 = (idx >> 5) & 7;
                int g3 = (idx >> 2) & 7;
                int b2 = idx & 3;
                out += ";2;";
                out += std::to_string(r3 * 100 / 7);
                out += ';';
                out += std::to_string(g3 * 100 / 7);
                out += ';';
                out += std::to_string(b2 * 100 / 3);
            }

            int run = 1;
            for (int x = 1; x < w; ++x) {
                if (v[x] == v[x - 1]) {
                    ++run;
                    continue;
                }
                emit_run(out, v[x - 1], run);
                run = 1;
            }
            emit_run(out, v[w - 1], run);
            out += '$';  // graphics carriage return (back to column 0)
        }
        out += '-';  // graphics new line (next 6-pixel band)

        if (out.size() > (1u << 20)) {
            write_all(out_fd, out.data(), out.size());
            out.clear();
        }
    }

    out += "\x1b\\";  // ST (end of DCS)
    write_all(out_fd, out.data(), out.size());
}

}  // namespace sixel
