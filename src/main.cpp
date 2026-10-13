#include "display.h"
#include "app.h"
#include "ansi.h"
#include "capture.h"
#include "kitty.h"
#include "sixel.h"
#include "term.h"
#include "input.h"
#include "wayland_server.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <langinfo.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <wchar.h>

namespace {
std::atomic<bool> g_stop{false};
Terminal* g_term = nullptr;

void on_stop(int) {
    g_stop.store(true);
}

// Ctrl+Z: give the terminal back before yielding to the shell's stop.
void on_tstp(int) {
    if (g_term) g_term->suspend();
    struct sigaction def;
    std::memset(&def, 0, sizeof(def));
    def.sa_handler = SIG_DFL;
    sigaction(SIGTSTP, &def, nullptr);
    raise(SIGTSTP);
}

// Ctrl+Z resume: re-acquire the terminal and re-arm the suspend handler.
void on_cont(int) {
    if (g_term) g_term->resume();
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_tstp;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &sa, nullptr);
}

// Safety net for normal exits/exit() calls.
void at_exit_restore() {
    if (g_term) g_term->restore();
}

void install_handlers() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop;
    sa.sa_flags = SA_RESTART;  // don't interrupt blocking X calls
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
    sigaction(SIGQUIT, &sa, nullptr);

    struct sigaction tstp;
    std::memset(&tstp, 0, sizeof(tstp));
    tstp.sa_handler = on_tstp;
    tstp.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &tstp, nullptr);

    struct sigaction cont;
    std::memset(&cont, 0, sizeof(cont));
    cont.sa_handler = on_cont;
    cont.sa_flags = SA_RESTART;
    sigaction(SIGCONT, &cont, nullptr);
}

struct Options {
    int fps = 30;
    int display_num = -1;
    int screen_w = 1920;
    int screen_h = 1080;
    double scale = 1.0;
    bool fullscreen = false;      // use the alternate screen as a workspace
    bool force_sixel = false;     // use sixel even if Kitty is available
    bool force_ansi = false;      // use the ANSI half-block fallback
    std::string connect_display;  // use existing X server, spawn nothing
    std::vector<std::string> program;
};

void print_usage() {
    std::cout <<
        "tender - render a GUI program's output to the terminal (kitty protocol)\n\n"
        "Usage:\n"
        "  tender [OPTIONS] -- <program> [args...]\n\n"
        "Options:\n"
        "  --fps N          Target frames per second (default 30)\n"
        "  --size WxH       Max virtual screen size (default 1920x1080); the\n"
        "                   program keeps its own resolution\n"
        "  --display N      Use a specific X display number for the Xvfb server\n"
        "  --connect :N     Use an already running X server (spawns nothing)\n"
        "  --scale F        Scale image within terminal cells (default 1.0)\n"
        "  -f, --fullscreen Use the alternate screen as a full-terminal workspace\n"
        "  --sixel          Force the sixel renderer (skip Kitty auto-detection)\n"
        "  --ansi           Force the ANSI half-block fallback renderer\n"
        "  -h, --help       Show this help\n\n"
        "The program runs on a private X server (Xvfb) and a built-in headless\n"
        "Wayland compositor at the same time; it picks whichever it prefers and\n"
        "windows from both are detected automatically.\n";
}

bool parse_size(const std::string& s, int& w, int& h) {
    auto x = s.find('x');
    if (x == std::string::npos) return false;
    w = std::atoi(s.substr(0, x).c_str());
    h = std::atoi(s.substr(x + 1).c_str());
    return w > 0 && h > 0;
}

bool parse_args(int argc, char** argv, Options& opt) {
    int i = 1;
    bool after_sep = false;
    for (; i < argc; ++i) {
        std::string a = argv[i];
        if (after_sep) {
            opt.program.push_back(a);
            continue;
        }
        if (a == "--") {
            after_sep = true;
        } else if (a == "-h" || a == "--help") {
            print_usage();
            std::exit(0);
        } else if (a == "--fps" && i + 1 < argc) {
            opt.fps = std::atoi(argv[++i]);
            if (opt.fps <= 0) opt.fps = 30;
        } else if (a == "--display" && i + 1 < argc) {
            opt.display_num = std::atoi(argv[++i]);
        } else if (a == "--connect" && i + 1 < argc) {
            opt.connect_display = argv[++i];
        } else if (a == "-f" || a == "--fullscreen") {
            opt.fullscreen = true;
        } else if (a == "--sixel") {
            opt.force_sixel = true;
        } else if (a == "--ansi") {
            opt.force_ansi = true;
        } else if (a == "--scale" && i + 1 < argc) {
            opt.scale = std::atof(argv[++i]);
        } else if (a == "--size" && i + 1 < argc) {
            if (!parse_size(argv[++i], opt.screen_w, opt.screen_h)) {
                std::cerr << "tender: invalid size\n";
                return false;
            }
        } else if (!a.empty() && a[0] == '-' && a != "-") {
            std::cerr << "tender: unknown option: " << a << "\n";
            return false;
        } else {
            opt.program.push_back(a);
            after_sep = true;
        }
    }
    return !opt.program.empty();
}

// Characters used to draw a window border.
struct BorderStyle {
    std::string tl, tr, bl, br, h, v;
};

BorderStyle make_border_style(bool utf8) {
    if (utf8) return {"\u256d", "\u256e", "\u2570", "\u256f", "\u2500", "\u2502"};
    return {"+", "+", "+", "+", "-", "|"};
}

// Whether the current locale is UTF-8 (rounded box drawing is safe).
bool terminal_supports_utf8() {
    const char* codeset = nl_langinfo(CODESET);
    if (codeset) {
        std::string cs = codeset;
        if (cs.find("UTF") != std::string::npos || cs.find("utf") != std::string::npos) {
            return true;
        }
        // Classic C / ASCII locales -> plain ASCII borders.
        if (cs == "ANSI_X3.4-1968" || cs == "US-ASCII" || cs == "ASCII") return false;
    }
    return true;  // terminals are UTF-8 far more often than not
}

std::string repeat(const std::string& s, int n) {
    std::string out;
    if (n > 0) out.reserve(s.size() * static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

// Decode one UTF-8 code point; advances `i`. Invalid bytes are returned as-is.
uint32_t utf8_next(const std::string& s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { ++i; return c; }
    int len = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
    else { ++i; return c; }
    if (i + static_cast<size_t>(len) > s.size()) { ++i; return c; }
    for (int k = 1; k < len; ++k) {
        unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) { ++i; return c; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    i += static_cast<size_t>(len);
    return cp;
}

int cp_width(uint32_t cp) {
    int w = wcwidth(static_cast<wchar_t>(cp));
    return w < 0 ? 1 : w;  // unknown -> assume one cell
}

// Printable, single-line version of a title.
std::string clean_title(const std::string& title) {
    std::string clean;
    for (unsigned char ch : title) {
        clean += ((ch >= 0x20 && ch != 0x7f) ? static_cast<char>(ch) : ' ');
    }
    return clean;
}

// Truncate to at most `max_cells` display columns (UTF-8 aware).
std::string truncate_cells(const std::string& s, int max_cells) {
    std::string out;
    int used = 0;
    size_t i = 0;
    while (i < s.size() && used < max_cells) {
        size_t start = i;
        uint32_t cp = utf8_next(s, i);
        int w = cp_width(cp);
        if (used + w > max_cells) break;
        out.append(s, start, i - start);
        used += w;
    }
    return out;
}

// Draw a window border (interior cols x rows cells, top-left at (top,left)),
// with `title` shown in the top edge.
void draw_box(int fd, int top, int left, int cols, int rows,
              const BorderStyle& bs, const std::string& title,
              int scr_cols, int scr_rows) {
    std::string top_edge;
    if (!title.empty() && cols > 0) {
        std::string label = " " + clean_title(title) + " ";
        label = truncate_cells(label, cols);
        int lw = 0;
        for (size_t i = 0; i < label.size();) lw += cp_width(utf8_next(label, i));
        int remaining = cols - lw;
        if (remaining < 0) remaining = 0;
        int left_dash = remaining / 2;
        int right_dash = remaining - left_dash;
        top_edge = repeat(bs.h, left_dash) + label + repeat(bs.h, right_dash);
    } else {
        top_edge = repeat(bs.h, cols);
    }

    (void)scr_cols;
    std::string s;
    auto pos = [&](int r, int c) {
        s += "\x1b[" + std::to_string(r) + ";" + std::to_string(c) + "H";
    };
    auto inrow = [&](int r) { return r >= 1 && r <= scr_rows; };
    const int bottom = top + rows + 1;
    if (inrow(top)) {  // top edge (with title)
        pos(top, left);
        s += bs.tl;
        s += top_edge;
        s += bs.tr;
    }
    for (int i = 1; i <= rows; ++i) {
        int r = top + i;
        if (!inrow(r)) continue;
        pos(r, left);
        s += bs.v;
        pos(r, left + cols + 1);
        s += bs.v;
    }
    if (inrow(bottom)) {
        pos(bottom, left);
        s += bs.bl;
        s += repeat(bs.h, cols);
        s += bs.br;
    }
    if (s.empty()) return;
    ssize_t w = write(fd, s.data(), s.size());
    (void)w;
}

// Blank a rectangle of terminal cells with spaces. Used to wipe the whole
// previous grid (borders and titles) before laying out a new one, so nothing is
// left behind when windows move or close.
void clear_region(int fd, int top, int left, int cols, int rows,
                  int scr_cols = 0, int scr_rows = 0) {
    if (scr_cols > 0) {
        int c0 = std::max(left, 1);
        int c1 = std::min(left + cols - 1, scr_cols);
        if (c1 < c0) return;
        left = c0;
        cols = c1 - c0 + 1;
    }
    if (scr_rows > 0) {
        int r0 = std::max(top, 1);
        int r1 = std::min(top + rows - 1, scr_rows);
        if (r1 < r0) return;
        top = r0;
        rows = r1 - r0 + 1;
    }
    if (cols <= 0 || rows <= 0) return;
    std::string spaces(static_cast<size_t>(cols), ' ');
    std::string s;
    for (int i = 0; i < rows; ++i) {
        s += "\x1b[" + std::to_string(top + i) + ";" + std::to_string(left) + "H";
        s += spaces;
        if (s.size() > (1u << 16)) {
            ssize_t w = write(fd, s.data(), s.size());
            (void)w;
            s.clear();
        }
    }
    if (!s.empty()) {
        ssize_t w = write(fd, s.data(), s.size());
        (void)w;
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        print_usage();
        return 1;
    }

    install_handlers();
    std::atexit(at_exit_restore);
    std::setlocale(LC_CTYPE, "");
    const BorderStyle border = make_border_style(terminal_supports_utf8());
    const bool utf8_border = border.tl.size() > 1;

    // 1. Backends: bring up both a private X server and the built-in headless
    //    Wayland compositor. The program picks whichever toolkit backend it
    //    prefers, and windows from either are detected continuously below.
    IDisplayServer* display = nullptr;
    WaylandServer wl(opt.screen_w, opt.screen_h);
    std::string display_name;
    bool x_ready = false;

    if (!opt.connect_display.empty()) {
        display = DisplayServerFactory::create_existing(opt.connect_display);
        if (!display->start()) {
            std::cerr << "tender: cannot connect to display "
                      << opt.connect_display << "\n";
            delete display;
            return 1;
        }
        display_name = display->display_name();
        x_ready = true;
    } else {
        display = DisplayServerFactory::create_xvfb(opt.display_num, opt.screen_w, opt.screen_h);
        if (display->start()) {
            display_name = display->display_name();
            x_ready = true;
        } else {
            std::cerr << "tender: no virtual X server (Xvfb); continuing with "
                         "Wayland only.\nInstall it (e.g. 'sudo apt install xvfb').\n";
            delete display;
            display = nullptr;
        }
    }

    // Start the built-in Wayland compositor as well (no-op if not compiled in).
    bool wl_ready = false;
#if HAVE_WAYLAND
    wl_ready = wl.start();
    if (!wl_ready) {
        std::cerr << "tender: built-in Wayland compositor failed to start; "
                     "continuing with X11 only.\n";
    }
#endif
    if (!wl_ready) {
        // Don't let the child fall back to a real Wayland session we don't host.
        unsetenv("WAYLAND_DISPLAY");
    }

    if (!x_ready && !wl_ready) {
        std::cerr << "tender: no usable display backend (X11 or Wayland)\n";
        if (display) { display->stop(); delete display; }
        return 1;
    }

    // 2. Launch the target program.
    App app(display_name);
    if (!app.launch(opt.program)) {
        std::cerr << "tender: failed to launch: " << opt.program.front() << "\n";
        if (display) { display->stop(); delete display; }
        wl.stop();
        return 1;
    }

    // 3. Input forwarding. The target window(s) are discovered continuously from
    //    inside the render loop below; we deliberately do not block here. The
    //    program may map its window late, or open new ones at any time.

    InputForwarder input(display_name);
    input.set_wayland(&wl);
    bool input_ok = input.init();
    if (!input_ok) {
        std::cerr << "tender: input forwarding unavailable; display only\n";
    }

    Terminal term;
    g_term = &term;
    term.set_mouse(input_ok);
    if (input_ok) {
        bool pixel = term.probe_pixel_mouse();
        term.set_pixel_mouse(pixel);
        input.set_pixel_mouse(pixel);
    }
    // Bitmap graphics: prefer Kitty, then sixel. --sixel/--ansi force a
    // renderer; otherwise bail out when the terminal supports neither.
    Terminal::Graphics gfx = term.detect_graphics();
    if (opt.force_sixel) {
        gfx = Terminal::Graphics::Sixel;
    } else if (opt.force_ansi) {
        gfx = Terminal::Graphics::Ansi;
    }
    if (gfx == Terminal::Graphics::Unsupported) {
        std::cerr << "tender: this terminal supports neither the Kitty "
                     "graphics protocol nor sixel.\n"
                     "Use --ansi for a low-resolution (half-block) fallback, "
                     "or run in a graphics-capable terminal.\n";
        input.close();
        app.stop();
        if (display) { display->stop(); delete display; }
        wl.stop();
        return 1;
    }
    int bg_r = 0, bg_g = 0, bg_b = 0;
    if (gfx == Terminal::Graphics::Sixel) term.query_background(bg_r, bg_g, bg_b);
    term.set_fullscreen(opt.fullscreen);
    term.init();

    // Anchor the tiled grid where the cursor currently is.
    int anchor_row = 1, anchor_col = 1;
    term.query_cursor_position(anchor_row, anchor_col);

    auto frame_interval = std::chrono::microseconds(1000000 / opt.fps);
    char inbuf[4096];
    int term_rows = 1;

    std::unique_ptr<Capturer> capturer;
    if (x_ready) capturer.reset(new Capturer(display_name));

    // One displayed window: an X11 top-level or a Wayland toplevel. Windows are
    // tiled in a grid, each with its own ASCII border and kitty image.
    struct WindowView {
        bool wayland = false;
        unsigned long id = 0;
        uint32_t image_id = 0;
        bool seen = false;
        bool captured = false;
        bool resizable = false;
        bool can_close = false;
        std::string title;
        Frame frame;           // last captured pixels
        Frame scaled;          // resized for the tile (when smaller)
        const Frame* disp = nullptr;
        int ox = 0, oy = 0;    // window top-left in root coords (X11)
        // Layout, in terminal cells. top/left is the border corner.
        int top = 0, left = 0;
        int img_cols = 0, img_rows = 0;
        int disp_w = 0, disp_h = 0;       // scaled image size in pixels
        int off_x = 0, off_y = 0;         // centring offset within the cell
        int last_fw = -1, last_fh = -1;   // last captured frame size
        // On-screen placement after clipping against the terminal edges.
        bool visible = false;
        int place_row = 0, place_col = 0, cell_off_x = 0, cell_off_y = 0;
        int src_x = 0, src_y = 0, vis_w = 0, vis_h = 0;
        // Free floating (dragged by its titlebar and dropped anywhere).
        bool floating = false;
        int float_left = 0, float_top = 0;
        // Last drawn border rectangle, for per-window erase.
        bool was_drawn = false;
        int last_top = 0, last_left = 0, last_img_cols = 0, last_img_rows = 0;
        // Last sixel placement, so it can be erased with a background fill.
        bool six_placed = false;
        int six_row = 0, six_col = 0, six_w = 0, six_h = 0;
    };
    std::vector<WindowView> windows;
    uint32_t next_image_id = 1;

    int grid_top = anchor_row, grid_left = anchor_col;
    int grid_rows = 0, grid_cols = 0;
    int last_cols = 0, last_rows = 0;
    bool layout_dirty = true;
    bool wayland_active = false;
    int last_hover = -1;

    // Resize-handle drag state (bottom-right corner of a tile). Positions are
    // kept in terminal pixels so they stay consistent with the tile geometry.
    double cur_cell_w = 10.0, cur_cell_h = 20.0;
    bool cur_pixel_mouse = false;
    int resize_idx = -1, resize_w0 = 0, resize_h0 = 0;
    double resize_mx0 = 0.0, resize_my0 = 0.0;
    bool resize_held = false;
    double resize_mx = 0.0, resize_my = 0.0;
    int resize_extra_w = 0, resize_extra_h = 0;

    // Vertical grid viewport scroll (rows hidden above the screen). 0 = top.
    int view_scroll = 0, view_max_scroll = 0;

    // Titlebar drag (moves a window out of the grid into a free position).
    int move_idx = -1;
    bool move_held = false;
    double move_grab_dx = 0.0, move_grab_dy = 0.0;

    auto find_window = [&](bool wl_backend, unsigned long id) -> WindowView* {
        for (auto& w : windows)
            if (w.wayland == wl_backend && w.id == id) return &w;
        return nullptr;
    };

    // Bitmap graphics: Kitty has image IDs, so images can be deleted; sixel is
    // drawn in place and has to be cleared by overwriting with spaces.
    auto draw_image = [&](const Frame& f, uint32_t id, int off_x, int off_y,
                          int src_x, int src_y, int vis_w, int vis_h) {
        if (gfx == Terminal::Graphics::Sixel) {
            // Sixel has no placement crop, so crop the frame ourselves.
            if (src_x != 0 || src_y != 0 || vis_w != f.width || vis_h != f.height) {
                Frame c;
                c.width = vis_w;
                c.height = vis_h;
                c.rgb.resize(static_cast<size_t>(vis_w) * vis_h * 3);
                for (int y = 0; y < vis_h; ++y) {
                    const uint8_t* srow =
                        f.rgb.data() + (static_cast<size_t>(src_y + y) * f.width + src_x) * 3;
                    uint8_t* drow = c.rgb.data() + static_cast<size_t>(y) * vis_w * 3;
                    std::memcpy(drow, srow, static_cast<size_t>(vis_w) * 3);
                }
                sixel::transmit_rgb(term.out_fd(), c);
            } else {
                sixel::transmit_rgb(term.out_fd(), f);
            }
        } else {
            // Only send the source-crop keys when actually clipping; some Kitty
            // versions reject unknown/inapplicable keys and drop the image.
            int sw = 0, sh = 0;
            if (src_x != 0 || src_y != 0 || vis_w != f.width || vis_h != f.height) {
                sw = vis_w;
                sh = vis_h;
            }
            kitty::transmit_rgb(term.out_fd(), f, id, 0, off_x, off_y,
                                src_x, src_y, sw, sh);
        }
    };
    auto clear_image = [&](WindowView& w) {
        if (gfx == Terminal::Graphics::Sixel) {
            // Sixel isn't removed by text in every terminal, so paint a solid
            // background sixel over the area the image occupied.
            if (w.six_placed) {
                std::string mv = "\x1b[" + std::to_string(w.six_row) + ";" +
                                 std::to_string(w.six_col) + "H";
                ssize_t wr = write(term.out_fd(), mv.data(), mv.size());
                (void)wr;
                sixel::clear(term.out_fd(), w.six_w, w.six_h, bg_r, bg_g, bg_b);
                w.six_placed = false;
            }
        } else if (gfx == Terminal::Graphics::Kitty) {
            kitty::delete_image(term.out_fd(), w.image_id);
        }
        // Ansi: nothing to delete; clear_region wipes the text.
    };

    auto do_resize = [&](WindowView& w, int nw, int nh) {
        if (w.wayland) {
            wl.resize_toplevel(w.id, nw, nh);
        } else if (capturer) {
            capturer->resize_window(w.id, nw, nh);
        }
    };
    auto do_close = [&](WindowView& w) {
        if (w.wayland) {
            wl.close_toplevel(w.id);
        } else if (capturer) {
            capturer->close_window(w.id);
        }
    };

    // Bottom-right resize handle: dragging it resizes the underlying window.
    // Work in terminal pixels (mx/my) so the tile bounds and the pointer use
    // the same scale and fractional cell widths can't shift the hit test.
    input.set_mouse_hook([&](int cb, int cx, int cy, char term) -> bool {
        double mx = (cx - 1) * (cur_pixel_mouse ? 1.0 : cur_cell_w);
        double my = (cy - 1) * (cur_pixel_mouse ? 1.0 : cur_cell_h);
        bool wheel = (cb & 64) != 0;
        int btn = cb & 3;
        if (resize_idx >= 0) {
            if (term == 'm' || resize_idx >= static_cast<int>(windows.size())) {
                resize_idx = -1;
                resize_held = false;
                return true;
            }
            resize_mx = mx;
            resize_my = my;
            return true;
        }
        if (move_held) {
            if (term == 'm' || move_idx >= static_cast<int>(windows.size())) {
                move_held = false;
                move_idx = -1;
                return true;
            }
            WindowView& w = windows[move_idx];
            int nleft = static_cast<int>(std::floor((mx - move_grab_dx) / cur_cell_w)) + 1;
            int ntop = static_cast<int>(std::floor((my - move_grab_dy) / cur_cell_h)) + 1;
            if (nleft < 1) nleft = 1;
            if (ntop < 1) ntop = 1;
            if (nleft != w.float_left || ntop != w.float_top) {
                w.float_left = nleft;
                w.float_top = ntop;
                layout_dirty = true;
            }
            return true;
        }
        if (wheel) {
            // Wheel over a window's border scrolls the grid viewport (only when
            // the grid is taller than the terminal). Over the image it is
            // forwarded to the app as usual.
            if (view_max_scroll > 0) {
                bool over_border = false;
                for (const auto& w : windows) {
                    if (w.img_cols <= 0 || w.img_rows <= 0) continue;
                    double tx0 = (w.left - 1) * cur_cell_w;
                    double tx1 = (w.left + w.img_cols + 1) * cur_cell_w;
                    double ty0 = (w.top - 1) * cur_cell_h;
                    double ty1 = (w.top + w.img_rows + 1) * cur_cell_h;
                    double ix0 = w.left * cur_cell_w;
                    double ix1 = (w.left + w.img_cols) * cur_cell_w;
                    double iy0 = w.top * cur_cell_h;
                    double iy1 = (w.top + w.img_rows) * cur_cell_h;
                    bool in_tile = mx >= tx0 && mx < tx1 && my >= ty0 && my < ty1;
                    bool in_image = mx >= ix0 && mx < ix1 && my >= iy0 && my < iy1;
                    if (in_tile && !in_image) { over_border = true; break; }
                }
                if (over_border) {
                    if (cb & 1) {
                        view_scroll = std::min(view_max_scroll, view_scroll + 3);
                    } else {
                        view_scroll = std::max(0, view_scroll - 3);
                    }
                    layout_dirty = true;
                    return true;
                }
            }
            return false;
        }
        if (term == 'M' && !wheel && btn == 0 && cb < 32) {  // press
            // Close button (top-right 2x2 cells), if supported.
            for (auto& w : windows) {
                if (!w.can_close || w.img_cols <= 0 || w.img_rows <= 0) continue;
                double x0 = (w.left + w.img_cols - 1) * cur_cell_w;
                double x1 = (w.left + w.img_cols + 1) * cur_cell_w;
                double y0 = (w.top - 1) * cur_cell_h;
                double y1 = (w.top + 1) * cur_cell_h;
                if (mx >= x0 && mx < x1 && my >= y0 && my < y1) {
                    do_close(w);
                    return true;
                }
            }
            for (int k = 0; k < static_cast<int>(windows.size()); ++k) {
                const WindowView& w = windows[k];
                if (!w.resizable || w.img_cols <= 0 || w.img_rows <= 0) continue;
                // Bottom-right 2x2 cells (interior corner + border corner).
                double x0 = (w.left + w.img_cols) * cur_cell_w;
                double x1 = (w.left + w.img_cols + 2) * cur_cell_w;
                double y0 = (w.top + w.img_rows) * cur_cell_h;
                double y1 = (w.top + w.img_rows + 2) * cur_cell_h;
                if (mx >= x0 && mx < x1 && my >= y0 && my < y1) {
                    resize_idx = k;
                    resize_mx0 = resize_mx = mx;
                    resize_my0 = resize_my = my;
                    resize_extra_w = resize_extra_h = 0;
                    resize_held = true;
                    resize_w0 = w.frame.width > 0 ? w.frame.width : w.disp_w;
                    resize_h0 = w.frame.height > 0 ? w.frame.height : w.disp_h;
                    return true;
                }
            }
            // Titlebar: press anywhere on the top border row starts a move.
            for (int k = 0; k < static_cast<int>(windows.size()); ++k) {
                const WindowView& w = windows[k];
                if (w.img_cols <= 0 || w.img_rows <= 0) continue;
                double y0 = (w.top - 1) * cur_cell_h;
                double y1 = w.top * cur_cell_h;
                double x0 = (w.left - 1) * cur_cell_w;
                double x1 = (w.left + w.img_cols + 1) * cur_cell_w;
                if (mx >= x0 && mx < x1 && my >= y0 && my < y1) {
                    move_idx = k;
                    move_held = true;
                    move_grab_dx = mx - (w.left - 1) * cur_cell_w;
                    move_grab_dy = my - (w.top - 1) * cur_cell_h;
                    windows[k].floating = true;
                    windows[k].float_left = w.left;
                    windows[k].float_top = w.top;
                    layout_dirty = true;
                    return true;
                }
            }
        }
        return false;
    });
    cur_pixel_mouse = term.pixel_mouse();

    while (!g_stop.load()) {
        auto t_start = std::chrono::steady_clock::now();

        if (!app.is_running()) break;

        // Always service terminal input, even when rendering falls behind and
        // the pacing wait-loop below is skipped.
        {
            size_t got = 0;
            while (term.poll_input(inbuf, sizeof(inbuf), 0, got) && got > 0) {
                if (input_ok) input.feed(inbuf, got);
                got = 0;
            }
        }

        // Continuously discover windows from both backends. Keep known windows
        // in place and append new ones; drop those that disappeared. Nothing
        // here blocks, so windows may appear at any time.
        wl.poll(0);
        std::vector<unsigned long> x_ids, wl_ids;
        if (capturer) capturer->discover_windows(x_ids);
        wl.toplevels(wl_ids);

        for (auto& w : windows) w.seen = false;
        auto add_window = [&](bool wl_backend, unsigned long id) {
            WindowView* w = find_window(wl_backend, id);
            if (!w) {
                windows.push_back(WindowView());
                w = &windows.back();
                w->wayland = wl_backend;
                w->id = id;
                w->image_id = next_image_id++;
                layout_dirty = true;
            }
            w->seen = true;
        };
        for (unsigned long id : x_ids) add_window(false, id);
        for (unsigned long id : wl_ids) add_window(true, id);

        for (size_t i = 0; i < windows.size();) {
            if (windows[i].seen) {
                ++i;
                continue;
            }
            WindowView& gone = windows[i];
            clear_image(gone);
            if (gone.was_drawn) {
                TermSize gts = term.size();
                clear_region(term.out_fd(), gone.last_top, gone.last_left,
                             gone.last_img_cols + 2, gone.last_img_rows + 2,
                             gts.cols, gts.rows);
            }
            windows.erase(windows.begin() + static_cast<std::ptrdiff_t>(i));
            layout_dirty = true;
        }

        // Capture every window.
        for (auto& w : windows) {
            Frame f;
            int x = 0, y = 0;
            bool ok = w.wayland
                          ? wl.capture_toplevel(w.id, f)
                          : (capturer && capturer->capture(w.id, f, &x, &y));
            if (ok) {
                if (f.width != w.last_fw || f.height != w.last_fh) {
                    w.last_fw = f.width;
                    w.last_fh = f.height;
                    layout_dirty = true;  // border must match the new size
                }
                w.frame = std::move(f);
                w.ox = x;
                w.oy = y;
                w.captured = true;
            }
            std::string title = w.wayland ? wl.toplevel_title(w.id)
                                          : (capturer ? capturer->window_title(w.id)
                                                      : std::string());
            if (title != w.title) {
                w.title = std::move(title);
                layout_dirty = true;  // redraw the border with the new title
            }
            bool resizable = w.wayland
                                 ? wl.toplevel_resizable(w.id)
                                 : (capturer ? capturer->window_resizable(w.id)
                                             : false);
            if (resizable != w.resizable) {
                w.resizable = resizable;
                layout_dirty = true;  // (re)draw or remove the resize handle
            }
            bool can_close = w.wayland
                                 ? wl.toplevel_can_close(w.id)
                                 : (capturer ? capturer->window_can_close(w.id)
                                             : false);
            if (can_close != w.can_close) {
                w.can_close = can_close;
                layout_dirty = true;  // (re)draw or remove the close button
            }
        }

        TermSize ts = term.size();
        term_rows = ts.rows;
        cur_cell_w = ts.cell_w;
        cur_cell_h = ts.cell_h;
        cur_pixel_mouse = term.pixel_mouse();
        if (ts.cols != last_cols || ts.rows != last_rows) {
            last_cols = ts.cols;
            last_rows = ts.rows;
            layout_dirty = true;
        }

        // Apply the resize drag every frame; while the pointer is held at the
        // bottom/right edge, keep growing (edge auto-scroll).
        if (resize_idx >= 0 && resize_held &&
            resize_idx < static_cast<int>(windows.size())) {
            if (resize_my >= (ts.rows - 1) * cur_cell_h) {
                resize_extra_h += static_cast<int>(cur_cell_h);
            }
            if (resize_mx >= (ts.cols - 1) * cur_cell_w) {
                resize_extra_w += static_cast<int>(cur_cell_w);
            }
            int nw = resize_w0 +
                     static_cast<int>(std::llround(resize_mx - resize_mx0)) +
                     resize_extra_w;
            int nh = resize_h0 +
                     static_cast<int>(std::llround(resize_my - resize_my0)) +
                     resize_extra_h;
            if (nw < 16) nw = 16;
            if (nh < 16) nh = 16;
            do_resize(windows[resize_idx], nw, nh);
        }

        const int n = static_cast<int>(windows.size());

        // (Re)compute the layout when the window set or terminal changes.
        if (layout_dirty) {
            // Erase what was drawn before, per window, so moved/floating tiles
            // are cleaned up exactly (a Kitty image is deleted, its cells wiped).
            for (auto& w : windows) {
                clear_image(w);
                if (w.was_drawn) {
                    clear_region(term.out_fd(), w.last_top, w.last_left,
                                 w.last_img_cols + 2, w.last_img_rows + 2,
                                 ts.cols, ts.rows);
                    w.was_drawn = false;
                }
            }

            // Native (unscaled) footprint of every captured window; the tiled
            // grid is sized to the largest non-floating window.
            int max_cols = 1, max_rows = 1, n_grid = 0;
            for (auto& w : windows) {
                if (!w.captured) {
                    w.img_cols = 0;
                    w.img_rows = 0;
                    w.disp = nullptr;
                    continue;
                }
                double s = opt.scale > 0.0 ? opt.scale : 1.0;
                w.disp_w = static_cast<int>(std::llround(w.frame.width * s));
                w.disp_h = static_cast<int>(std::llround(w.frame.height * s));
                if (w.disp_w < 1) w.disp_w = 1;
                if (w.disp_h < 1) w.disp_h = 1;
                // Border interior = ceil(graphic / cell); the image is centred
                // in the (slightly larger) interior via pixel offsets.
                w.img_cols = static_cast<int>(std::ceil(w.disp_w / ts.cell_w));
                w.img_rows = static_cast<int>(std::ceil(w.disp_h / ts.cell_h));
                if (w.img_cols < 1) w.img_cols = 1;
                if (w.img_rows < 1) w.img_rows = 1;
                w.off_x = static_cast<int>(std::llround(
                    (w.img_cols * ts.cell_w - w.disp_w) / 2.0));
                w.off_y = static_cast<int>(std::llround(
                    (w.img_rows * ts.cell_h - w.disp_h) / 2.0));
                if (w.off_x < 0) w.off_x = 0;
                if (w.off_y < 0) w.off_y = 0;
                if (!w.floating) {
                    ++n_grid;
                    if (w.img_cols > max_cols) max_cols = w.img_cols;
                    if (w.img_rows > max_rows) max_rows = w.img_rows;
                }
            }

            grid_cols = 0;
            grid_rows = 0;
            if (n_grid > 0) {
                int blk_cols = max_cols + 2;  // 2 cells for the border
                int blk_rows = max_rows + 2;
                if (blk_cols < 3) blk_cols = 3;
                if (blk_rows < 3) blk_rows = 3;

                // Choose the column count that best fits the terminal so tiles
                // don't run off-screen and overlap.
                int ideal = 1;
                while (ideal * ideal < n_grid) ++ideal;
                int gcols = 1;
                long best_score = 0;
                bool have_best = false;
                for (int g = 1; g <= n_grid; ++g) {
                    int r = (n_grid + g - 1) / g;
                    long w = static_cast<long>(g) * blk_cols;
                    long h = static_cast<long>(r) * blk_rows;
                    long over = 0;
                    if (w > ts.cols) over += (w - ts.cols);
                    if (h > ts.rows) over += (h - ts.rows);
                    long score = over * 1000 + std::labs(g - ideal);
                    if (!have_best || score < best_score) {
                        have_best = true;
                        best_score = score;
                        gcols = g;
                    }
                }
                int grows = (n_grid + gcols - 1) / gcols;

                int total_w = gcols * blk_cols;
                int total_h = grows * blk_rows;

                int origin_col = anchor_col;
                if (total_w <= ts.cols) {
                    if (origin_col + total_w - 1 > ts.cols) {
                        origin_col = ts.cols - total_w + 1;
                    }
                    if (origin_col < 1) origin_col = 1;
                } else {
                    origin_col = 1;  // too wide to fit; start at the left
                }

                int origin_row = anchor_row;
                if (total_h <= ts.rows) {
                    view_max_scroll = 0;
                    view_scroll = 0;
                    if (origin_row + total_h - 1 > ts.rows) {
                        // Scroll so the whole grid is visible, prompt above it.
                        int nlines = total_h + 1;
                        std::string nls(static_cast<size_t>(nlines), '\n');
                        ssize_t wn = write(term.out_fd(), nls.data(), nls.size());
                        (void)wn;
                        origin_row = ts.rows - total_h + 1;
                        if (origin_row < 1) origin_row = 1;
                    }
                } else {
                    // Taller than the terminal: the grid is scrollable. Keep the
                    // bottom pinned while resizing; else honour the view scroll.
                    view_max_scroll = total_h - ts.rows;
                    if (resize_held) view_scroll = view_max_scroll;
                    if (view_scroll > view_max_scroll) view_scroll = view_max_scroll;
                    if (view_scroll < 0) view_scroll = 0;
                    origin_row = 1 - view_scroll;
                }
                anchor_row = origin_row;
                anchor_col = origin_col;
                grid_top = origin_row;
                grid_left = origin_col;
                grid_rows = total_h;
                grid_cols = total_w;

                int slot = 0;
                for (auto& w : windows) {
                    if (!w.captured || w.floating) continue;
                    int r = slot / gcols;
                    int c = slot % gcols;
                    ++slot;
                    int block_left = origin_col + c * blk_cols;
                    int block_top = origin_row + r * blk_rows;
                    int slack_col = blk_cols - 2 - w.img_cols;
                    int slack_row = blk_rows - 2 - w.img_rows;
                    w.left = block_left + slack_col / 2;
                    w.top = block_top + slack_row / 2;
                }
            } else {
                view_max_scroll = 0;
                view_scroll = 0;
            }

            // Floating windows keep their manual position.
            for (auto& w : windows) {
                if (!w.captured || !w.floating) continue;
                w.left = w.float_left;
                w.top = w.float_top;
            }

            // Draw every captured window.
            for (auto& w : windows) {
                if (!w.captured) continue;
                draw_box(term.out_fd(), w.top, w.left, w.img_cols, w.img_rows,
                         border, w.title, ts.cols, ts.rows);
                int gr = w.top + w.img_rows + 1;
                int gc = w.left + w.img_cols + 1;
                if (w.resizable && gr >= 1 && gr <= ts.rows && gc >= 1 && gc <= ts.cols) {
                    std::string grip = "\x1b[" + std::to_string(gr) + ";" +
                                       std::to_string(gc) + "H";
                    grip += utf8_border ? "\u25e2" : "+";
                    ssize_t gw = write(term.out_fd(), grip.data(), grip.size());
                    (void)gw;
                }
                int cr = w.top;
                int cc = w.left + w.img_cols + 1;
                if (w.can_close && cr >= 1 && cr <= ts.rows && cc >= 1 && cc <= ts.cols) {
                    std::string btn = "\x1b[" + std::to_string(cr) + ";" +
                                      std::to_string(cc) + "H";
                    btn += utf8_border ? "\u2715" : "x";
                    ssize_t bw = write(term.out_fd(), btn.data(), btn.size());
                    (void)bw;
                }
                w.last_top = w.top;
                w.last_left = w.left;
                w.last_img_cols = w.img_cols;
                w.last_img_rows = w.img_rows;
                w.was_drawn = true;
            }
            layout_dirty = false;
        }

        // Scale and draw each window's image into its tile, clipping it against
        // the terminal edges when the grid is larger than the screen.
        const int scr_px_w = static_cast<int>(ts.cols * ts.cell_w);
        const int scr_px_h = static_cast<int>(ts.rows * ts.cell_h);
        for (auto& w : windows) {
            w.visible = false;
            if (!w.captured || w.img_cols <= 0 || w.img_rows <= 0) continue;
            if (w.disp_w != w.frame.width || w.disp_h != w.frame.height) {
                resize_frame(w.frame, w.scaled, w.disp_w, w.disp_h);
                w.disp = &w.scaled;
            } else {
                w.disp = &w.frame;
            }
            if (gfx == Terminal::Graphics::Ansi) {
                // Universal fallback: half-block text, no placement needed.
                ansi::transmit_rgb(term.out_fd(), *w.disp, w.top + 1, w.left + 1,
                                   w.img_cols, w.img_rows, ts.rows);
                w.place_col = w.left + 1;
                w.place_row = w.top + 1;
                w.cell_off_x = w.cell_off_y = 0;
                w.src_x = w.src_y = 0;
                w.vis_w = w.disp->width;
                w.vis_h = w.disp->height;
                w.visible = true;
                continue;
            }
            int ox = w.left * static_cast<int>(ts.cell_w) + w.off_x;
            int oy = w.top * static_cast<int>(ts.cell_h) + w.off_y;
            int sx = ox < 0 ? 0 : ox;
            int sy = oy < 0 ? 0 : oy;
            int src_x = sx - ox;
            int src_y = sy - oy;
            int vis_w = w.disp->width - src_x;
            int vis_h = w.disp->height - src_y;
            if (vis_w > scr_px_w - sx) vis_w = scr_px_w - sx;
            if (vis_h > scr_px_h - sy) vis_h = scr_px_h - sy;
            if (vis_w <= 0 || vis_h <= 0) continue;

            w.place_col = sx / static_cast<int>(ts.cell_w) + 1;
            w.place_row = sy / static_cast<int>(ts.cell_h) + 1;
            w.cell_off_x = sx % static_cast<int>(ts.cell_w);
            w.cell_off_y = sy % static_cast<int>(ts.cell_h);
            w.src_x = src_x;
            w.src_y = src_y;
            w.vis_w = vis_w;
            w.vis_h = vis_h;
            w.visible = true;

            std::string move = "\x1b[" + std::to_string(w.place_row) + ";" +
                               std::to_string(w.place_col) + "H";
            ssize_t wr = write(term.out_fd(), move.data(), move.size());
            (void)wr;
            draw_image(*w.disp, w.image_id, w.cell_off_x, w.cell_off_y,
                       src_x, src_y, vis_w, vis_h);
            if (gfx == Terminal::Graphics::Sixel) {
                w.six_placed = true;
                w.six_row = w.place_row;
                w.six_col = w.place_col;
                w.six_w = vis_w;
                w.six_h = vis_h;
            }
        }

        // Route input to the window under the mouse (else the first window).
        int hover = -1;
        int mc = 0, mr = 0;
        if (input_ok && input.last_mouse(mc, mr)) {
            for (int k = 0; k < n; ++k) {
                const WindowView& w = windows[k];
                if (mc >= w.left + 1 && mc <= w.left + w.img_cols &&
                    mr >= w.top + 1 && mr <= w.top + w.img_rows) {
                    hover = k;
                    break;
                }
            }
        }
        if (hover < 0 && n > 0) hover = 0;

        if (n > 0 && input_ok) {
            WindowView& w = windows[hover];
            if (w.wayland) {
                if (!wayland_active) {
                    wayland_active = true;
                    input.set_wayland_active(true);
                }
            } else {
                if (wayland_active) {
                    wayland_active = false;
                    input.set_wayland_active(false);
                }
                input.set_target(w.id);
                if (hover != last_hover) input.focus_target();
            }
            if (w.disp && w.visible) {
                input.update_geometry(w.frame.width, w.frame.height,
                                      w.disp->width, w.disp->height,
                                      ts.cell_w, ts.cell_h,
                                      w.place_col, w.place_row,
                                      w.wayland ? 0 : w.ox,
                                      w.wayland ? 0 : w.oy,
                                      w.cell_off_x, w.cell_off_y,
                                      w.vis_w, w.vis_h, w.src_x, w.src_y);
            }
        }
        last_hover = hover;

        // Move the cursor below the grid so following output stays beneath it.
        if (n > 0 && grid_rows > 0) {
            int below = grid_top + grid_rows;
            if (term_rows > 0 && below > term_rows) below = term_rows;
            std::string down = "\x1b[" + std::to_string(below) + ";1H";
            ssize_t wr2 = write(term.out_fd(), down.data(), down.size());
            (void)wr2;
        }

        // Wait out the rest of the frame interval, servicing events/input.
        auto deadline = t_start + frame_interval;
        while (!g_stop.load()) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) break;
            int remain = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
            if (remain < 1) remain = 1;
            const int wait_ms = remain > 20 ? 20 : remain;

            // Keep dispatching compositor protocol events (so Wayland clients
            // keep painting even while we show an X11 window), then wait for
            // terminal input for the remainder of the frame.
            wl.poll(0);
            size_t got = 0;
            bool had = term.poll_input(inbuf, sizeof(inbuf), wait_ms, got);
            if (had && got > 0) {
                if (input_ok) input.feed(inbuf, got);
            }
            if (!had) {
                // stdin may be at EOF / non-interactive: avoid a busy loop.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    // 5. Cleanup: remove every image and wipe every tile, then drop the cursor.
    TermSize cts = term.size();
    for (auto& w : windows) {
        clear_image(w);
        if (w.was_drawn) {
            clear_region(term.out_fd(), w.last_top, w.last_left,
                         w.last_img_cols + 2, w.last_img_rows + 2,
                         cts.cols, cts.rows);
        }
    }
    int row = grid_rows > 0 ? grid_top + grid_rows : anchor_row;
    if (term_rows > 0 && row > term_rows) row = term_rows;
    if (row < 1) row = 1;
    std::string mv = "\x1b[" + std::to_string(row) + ";1H";
    ssize_t wr = write(term.out_fd(), mv.data(), mv.size());
    (void)wr;
    term.restore();
    g_term = nullptr;
    input.close();
    app.stop();
    capturer.reset();  // close the X connection before the server goes away
    if (display) { display->stop(); delete display; }
    wl.stop();
    return 0;
}
