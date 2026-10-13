#include "kitty.h"
#include <zlib.h>
#include <unistd.h>
#include <cerrno>
#include <algorithm>
#include <cstring>
#include <sstream>

namespace kitty {

std::vector<uint8_t> zlib_compress(const uint8_t* data, size_t len) {
    uLong bound = compressBound(static_cast<uLong>(len));
    std::vector<uint8_t> out(bound);
    uLongf dest_len = bound;
    if (compress2(out.data(), &dest_len, data, static_cast<uLong>(len), Z_BEST_SPEED) != Z_OK) {
        return {};
    }
    out.resize(dest_len);
    return out;
}

static const char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(kBase64[(n >> 18) & 63]);
        out.push_back(kBase64[(n >> 12) & 63]);
        out.push_back(kBase64[(n >> 6) & 63]);
        out.push_back(kBase64[n & 63]);
    }
    if (i < len) {
        uint32_t n = data[i] << 16;
        bool two = (i + 1 < len);
        if (two) n |= data[i + 1] << 8;
        out.push_back(kBase64[(n >> 18) & 63]);
        out.push_back(kBase64[(n >> 12) & 63]);
        out.push_back(two ? kBase64[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

static void write_all(int fd, const char* data, size_t len) {
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

void transmit_rgb(int out_fd, const Frame& frame, uint32_t image_id, int cols,
                  int off_x, int off_y, int src_x, int src_y, int src_w, int src_h) {
    std::vector<uint8_t> compressed = zlib_compress(frame.rgb.data(), frame.rgb.size());
    if (compressed.empty()) {
        return;
    }
    std::string b64 = base64_encode(compressed.data(), compressed.size());
    if (b64.empty()) {
        return;
    }

    std::ostringstream ctrl;
    ctrl << "a=T,f=24,o=z,i=" << image_id
         << ",s=" << frame.width << ",v=" << frame.height;
    if (cols > 0) {
        ctrl << ",c=" << cols;
    }
    if (src_w > 0) ctrl << ",x=" << src_x << ",w=" << src_w;
    if (src_h > 0) ctrl << ",y=" << src_y << ",h=" << src_h;
    if (off_x > 0) ctrl << ",X=" << off_x;
    if (off_y > 0) ctrl << ",Y=" << off_y;
    ctrl << ",C=1,q=2";

    const size_t kChunk = 4096;
    size_t offset = 0;
    bool first = true;
    std::string seq;
    while (offset < b64.size()) {
        size_t take = std::min(kChunk, b64.size() - offset);
        // keep chunks multiples of 4 except the last
        if (offset + take < b64.size()) {
            take &= ~static_cast<size_t>(3);
            if (take == 0) take = kChunk;
        }
        bool more = (offset + take < b64.size());
        std::string control;
        if (first) {
            control = ctrl.str();
            first = false;
        }
        if (!control.empty()) {
            control += ",";
        }
        control += "m=";
        control += more ? "1" : "0";

        seq.clear();
        seq += "\x1b_G";
        seq += control;
        seq += ";";
        seq.append(b64, offset, take);
        seq += "\x1b\\";
        write_all(out_fd, seq.data(), seq.size());

        offset += take;
    }
}

void delete_image(int out_fd, uint32_t image_id) {
    std::ostringstream seq;
    seq << "\x1b_Ga=d,d=I,i=" << image_id << ",q=2\x1b\\";
    std::string s = seq.str();
    write_all(out_fd, s.data(), s.size());
}

void delete_all(int out_fd) {
    const char seq[] = "\x1b_Ga=d,d=A,q=2\x1b\\";
    write_all(out_fd, seq, sizeof(seq) - 1);
}

}  // namespace kitty
