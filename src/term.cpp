#include "term.h"
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>

Terminal::Terminal()
    : m_out_fd(STDOUT_FILENO), m_in_fd(STDIN_FILENO),
      m_initialized(false), m_have_termios(false), m_saved_termios(nullptr) {
}

Terminal::~Terminal() {
    restore();
}

void Terminal::apply_raw() {
    if (isatty(m_in_fd)) {
        if (!m_have_termios) {
            struct termios* saved = new termios();
            if (tcgetattr(m_in_fd, saved) == 0) {
                m_saved_termios = saved;
                m_have_termios = true;
            } else {
                delete saved;
            }
        }
        if (m_have_termios) {
            struct termios raw = *static_cast<struct termios*>(m_saved_termios);
            // Keep ISIG so Ctrl+C / Ctrl+Z still generate signals, but disable
            // canonical mode and echo for a raw-ish display.
            raw.c_lflag &= ~(ICANON | ECHO);
            raw.c_iflag &= ~(IXON | ICRNL);
            raw.c_oflag &= ~(OPOST);
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            tcsetattr(m_in_fd, TCSANOW, &raw);
        }
    }

    // In fullscreen mode enter the alternate screen and clear it so the whole
    // terminal becomes a workspace. Disable autowrap (DECAWM) so drawing into
    // the last column/row cannot trigger a wrap/scroll, and hide the cursor.
    std::string seq;
    if (m_fullscreen) seq += "\x1b[?1049h\x1b[2J\x1b[H";
    seq += "\x1b[?7l\x1b[?25l";
    ssize_t r = ::write(m_out_fd, seq.data(), seq.size());
    (void)r;

    if (m_mouse) {
        // Button, drag and hover (any-motion) tracking with SGR encoding.
        std::string mice = "\x1b[?1000h\x1b[?1002h\x1b[?1003h\x1b[?1006h";
        if (m_pixel_mouse) {
            mice += "\x1b[?1016h";  // SGR-Pixels: coordinates in pixels
        }
        ssize_t r2 = ::write(m_out_fd, mice.data(), mice.size());
        (void)r2;
    }
    m_initialized = true;
}

void Terminal::revert() {
    if (m_mouse) {
        std::string mice = "\x1b[?1006l\x1b[?1003l\x1b[?1002l\x1b[?1000l";
        if (m_pixel_mouse) {
            mice += "\x1b[?1016l";
        }
        ssize_t rm = ::write(m_out_fd, mice.data(), mice.size());
        (void)rm;
    }
    // Show the cursor, re-enable autowrap, and (in fullscreen mode) leave the
    // alternate screen to restore the user's previous terminal content.
    std::string seq = "\x1b[?25h\x1b[?7h";
    if (m_fullscreen) seq += "\x1b[?1049l";
    ssize_t r = ::write(m_out_fd, seq.data(), seq.size());
    (void)r;

    if (m_have_termios && m_saved_termios) {
        tcsetattr(m_in_fd, TCSANOW, static_cast<struct termios*>(m_saved_termios));
    }
    // Drop any keys typed while we owned the terminal so the shell doesn't
    // receive stale bytes.
    if (isatty(m_in_fd)) {
        tcflush(m_in_fd, TCIFLUSH);
    }
    m_initialized = false;
}

bool Terminal::init() {
    if (m_initialized) return true;
    apply_raw();
    return true;
}

void Terminal::restore() {
    if (m_initialized) {
        revert();
    }
    if (m_have_termios && m_saved_termios) {
        delete static_cast<struct termios*>(m_saved_termios);
        m_saved_termios = nullptr;
        m_have_termios = false;
    }
}

void Terminal::suspend() {
    if (m_initialized) {
        revert();
    }
}

void Terminal::resume() {
    if (!m_initialized) {
        apply_raw();
    }
}

int Terminal::out_fd() const { return m_out_fd; }
int Terminal::in_fd() const { return m_in_fd; }

bool Terminal::query_cursor_position(int& row, int& col) {
    row = 1;
    col = 1;
    if (!isatty(m_in_fd)) return false;

    struct termios saved;
    if (tcgetattr(m_in_fd, &saved) != 0) return false;
    struct termios raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(m_in_fd, TCSANOW, &raw) != 0) return false;

    // Device Status Report: CSI 6 n -> reply CSI row ; col R
    const char query[] = "\x1b[6n";
    ssize_t w = ::write(m_out_fd, query, sizeof(query) - 1);
    (void)w;

    std::string resp;
    bool done = false;
    for (int i = 0; i < 20 && !done; ++i) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(m_in_fd, &fds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10000;  // 10ms
        int r = select(m_in_fd + 1, &fds, nullptr, nullptr, &tv);
        if (r > 0) {
            char buf[64];
            ssize_t n = ::read(m_in_fd, buf, sizeof(buf));
            if (n > 0) {
                resp.append(buf, static_cast<size_t>(n));
                if (resp.find('R') != std::string::npos) done = true;
            }
        } else if (r < 0 && errno == EINTR) {
            continue;
        }
    }
    tcsetattr(m_in_fd, TCSANOW, &saved);

    size_t pos = resp.find("\x1b[");
    if (pos == std::string::npos) return false;
    pos += 2;
    char* end = nullptr;
    long rr = std::strtol(resp.c_str() + pos, &end, 10);
    if (!end || *end != ';') return false;
    long cc = std::strtol(end + 1, &end, 10);
    if (rr <= 0 || cc <= 0) return false;
    row = static_cast<int>(rr);
    col = static_cast<int>(cc);
    return true;
}

bool Terminal::probe_pixel_mouse() {
    if (!isatty(m_in_fd)) return false;

    struct termios saved;
    if (tcgetattr(m_in_fd, &saved) != 0) return false;
    struct termios raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(m_in_fd, TCSANOW, &raw) != 0) return false;

    // DECRQM for mode 1016 (SGR-Pixels): reply is CSI ? 1016 ; Ps $ y
    const char query[] = "\x1b[?1016$p";
    ssize_t w = ::write(m_out_fd, query, sizeof(query) - 1);
    (void)w;

    std::string resp;
    bool done = false;
    for (int i = 0; i < 20 && !done; ++i) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(m_in_fd, &fds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10000;  // 10ms
        int r = select(m_in_fd + 1, &fds, nullptr, nullptr, &tv);
        if (r > 0) {
            char buf[128];
            ssize_t n = ::read(m_in_fd, buf, sizeof(buf));
            if (n > 0) {
                resp.append(buf, static_cast<size_t>(n));
                if (resp.find("$y") != std::string::npos) done = true;
            }
        } else if (r < 0 && errno == EINTR) {
            continue;
        }
    }
    tcsetattr(m_in_fd, TCSANOW, &saved);

    size_t pos = resp.find("1016;");
    if (pos == std::string::npos) return false;
    pos += 5;
    int ps = std::atoi(resp.c_str() + pos);
    return ps != 0;  // 1 = set, 2 = reset, 0 = not recognized
}

bool Terminal::query_background(int& r, int& g, int& b) {
    r = g = b = 0;
    if (!isatty(m_in_fd)) return false;

    struct termios saved;
    if (tcgetattr(m_in_fd, &saved) != 0) return false;
    struct termios raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(m_in_fd, TCSANOW, &raw) != 0) return false;

    const char query[] = "\x1b]11;?\x1b\\";
    ssize_t w = ::write(m_out_fd, query, sizeof(query) - 1);
    (void)w;

    std::string resp;
    for (int i = 0; i < 20; ++i) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(m_in_fd, &fds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10000;
        int rr = select(m_in_fd + 1, &fds, nullptr, nullptr, &tv);
        if (rr > 0) {
            char buf[128];
            ssize_t n = ::read(m_in_fd, buf, sizeof(buf));
            if (n > 0) {
                resp.append(buf, static_cast<size_t>(n));
                if (resp.find("11;") != std::string::npos &&
                    (resp.find('\x07') != std::string::npos ||
                     resp.find("\x1b\\") != std::string::npos)) {
                    break;
                }
            }
        } else if (rr < 0 && errno == EINTR) {
            --i;
        }
    }

    tcsetattr(m_in_fd, TCSANOW, &saved);

    size_t p = resp.find("rgb:");
    if (p == std::string::npos) return false;
    p += 4;
    int vals[3] = {0, 0, 0};
    for (int k = 0; k < 3; ++k) {
        size_t e = p;
        while (e < resp.size() && resp[e] != '/' && resp[e] != '\x07' &&
               resp[e] != '\x1b' && resp[e] != ';') {
            ++e;
        }
        std::string hex = resp.substr(p, e - p);
        if (hex.empty()) return false;
        long v = std::strtol(hex.c_str(), nullptr, 16);
        if (hex.size() >= 4) v >>= 8;  // 16-bit -> 8-bit
        if (v > 255) v = 255;
        vals[k] = static_cast<int>(v);
        p = (e < resp.size()) ? e + 1 : e;
    }
    r = vals[0];
    g = vals[1];
    b = vals[2];
    return true;
}

Terminal::Graphics Terminal::detect_graphics() {
    m_graphics = Graphics::Unsupported;
    if (!isatty(m_in_fd)) return m_graphics;

    struct termios saved;
    if (tcgetattr(m_in_fd, &saved) != 0) return m_graphics;
    struct termios raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(m_in_fd, TCSANOW, &raw) != 0) return m_graphics;

    // Read until `needle` appears or we run out of tries.
    auto read_until = [&](std::string& resp, const char* needle, int tries) {
        for (int i = 0; i < tries; ++i) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(m_in_fd, &fds);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 10000;  // 10ms
            int r = select(m_in_fd + 1, &fds, nullptr, nullptr, &tv);
            if (r > 0) {
                char buf[256];
                ssize_t n = ::read(m_in_fd, buf, sizeof(buf));
                if (n > 0) {
                    resp.append(buf, static_cast<size_t>(n));
                    if (resp.find(needle) != std::string::npos) return;
                }
            } else if (r < 0 && errno == EINTR) {
                --i;
            }
        }
    };

    // Kitty graphics query: a supporting terminal answers with an APC reply.
    const char kq[] = "\x1b_Gi=31,a=q;\x1b\\";
    ssize_t w = ::write(m_out_fd, kq, sizeof(kq) - 1);
    (void)w;
    std::string kresp;
    read_until(kresp, "\x1b_G", 3);
    bool kitty = kresp.find("\x1b_G") != std::string::npos;

    bool sixel = false;
    if (!kitty) {
        // Primary Device Attributes: parameter 4 means sixel graphics.
        const char da[] = "\x1b[c";
        ssize_t w2 = ::write(m_out_fd, da, sizeof(da) - 1);
        (void)w2;
        std::string dresp;
        read_until(dresp, "c", 20);
        size_t p = dresp.find("\x1b[?");
        size_t e = (p == std::string::npos) ? std::string::npos : dresp.find('c', p);
        if (p != std::string::npos && e != std::string::npos) {
            std::string params = dresp.substr(p + 3, e - (p + 3));
            size_t start = 0;
            while (start <= params.size()) {
                size_t semi = params.find(';', start);
                std::string tok = params.substr(
                    start, semi == std::string::npos ? std::string::npos : semi - start);
                if (tok == "4") sixel = true;
                if (semi == std::string::npos) break;
                start = semi + 1;
            }
        }
    }

    tcsetattr(m_in_fd, TCSANOW, &saved);
    if (kitty) {
        m_graphics = Graphics::Kitty;
    } else if (sixel) {
        m_graphics = Graphics::Sixel;
    } else {
        m_graphics = Graphics::Unsupported;  // no supported protocol (use --ansi)
    }
    return m_graphics;
}

TermSize Terminal::size() const {
    TermSize s;
    struct winsize ws;
    std::memset(&ws, 0, sizeof(ws));
    if (ioctl(m_out_fd, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col > 0) s.cols = ws.ws_col;
        if (ws.ws_row > 0) s.rows = ws.ws_row;
        if (ws.ws_xpixel > 0 && ws.ws_col > 0) {
            s.cell_w = static_cast<double>(ws.ws_xpixel) / ws.ws_col;
        }
        if (ws.ws_ypixel > 0 && ws.ws_row > 0) {
            s.cell_h = static_cast<double>(ws.ws_ypixel) / ws.ws_row;
        }
    }
    if (s.cell_w <= 0.0) s.cell_w = 10.0;
    if (s.cell_h <= 0.0) s.cell_h = 20.0;
    return s;
}

bool Terminal::poll_input(char* buf, size_t n, int timeout_ms, size_t& got) {
    got = 0;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(m_in_fd, &fds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int r = select(m_in_fd + 1, &fds, nullptr, nullptr, &tv);
    if (r <= 0) return false;
    ssize_t rd = ::read(m_in_fd, buf, n);
    if (rd <= 0) return false;
    got = static_cast<size_t>(rd);
    return true;
}
