#include "input.h"
#include "wayland_server.h"

#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <dlfcn.h>
#include <cstring>
#include <cstdlib>

namespace {

struct Seq {
    const char* seq;
    KeySym ks;
};

// Common xterm-style key sequences.
const Seq kSeqs[] = {
    {"\x1b[A", XK_Up},      {"\x1b[B", XK_Down},
    {"\x1b[C", XK_Right},   {"\x1b[D", XK_Left},
    {"\x1b[H", XK_Home},    {"\x1b[F", XK_End},
    {"\x1bOH", XK_Home},    {"\x1bOF", XK_End},
    {"\x1b[1~", XK_Home},   {"\x1b[2~", XK_Insert},
    {"\x1b[3~", XK_Delete}, {"\x1b[4~", XK_End},
    {"\x1b[5~", XK_Prior},  {"\x1b[6~", XK_Next},
    {"\x1b[Z", XK_ISO_Left_Tab},
    {"\x1bOP", XK_F1},      {"\x1bOQ", XK_F2},
    {"\x1bOR", XK_F3},      {"\x1bOS", XK_F4},
    {"\x1b[15~", XK_F5},    {"\x1b[17~", XK_F6},
    {"\x1b[18~", XK_F7},    {"\x1b[19~", XK_F8},
    {"\x1b[20~", XK_F9},    {"\x1b[21~", XK_F10},
    {"\x1b[23~", XK_F11},   {"\x1b[24~", XK_F12},
};

bool parse_uint(const std::string& s, size_t& i, int& out) {
    size_t start = i;
    long v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        ++i;
    }
    if (i == start) return false;
    out = static_cast<int>(v);
    return true;
}

}  // namespace

InputForwarder::InputForwarder(const std::string& display_name)
    : m_display_name(display_name) {}

InputForwarder::~InputForwarder() {
    close();
}

void InputForwarder::close() {
    if (m_dpy) {
        XCloseDisplay(m_dpy);
        m_dpy = nullptr;
    }
    if (m_lib) {
        dlclose(m_lib);
        m_lib = nullptr;
    }
    m_available = false;
}

bool InputForwarder::init() {
    m_x11_ok = false;
    m_wl_ok = (m_wl != nullptr) && m_wl->input_available();

    // XTEST path: only if we have an X server to talk to.
    if (!m_display_name.empty()) {
        setenv("DISPLAY", m_display_name.c_str(), 1);
        m_dpy = XOpenDisplay(nullptr);
        if (m_dpy) {
            m_lib = dlopen("libXtst.so.6", RTLD_NOW | RTLD_LOCAL);
            if (m_lib) {
                m_query = reinterpret_cast<PFN_Query>(dlsym(m_lib, "XTestQueryExtension"));
                m_fake_key = reinterpret_cast<PFN_Key>(dlsym(m_lib, "XTestFakeKeyEvent"));
                m_fake_button = reinterpret_cast<PFN_Button>(dlsym(m_lib, "XTestFakeButtonEvent"));
                m_fake_motion = reinterpret_cast<PFN_Motion>(dlsym(m_lib, "XTestFakeMotionEvent"));
            }
            if (m_query && m_fake_key && m_fake_button && m_fake_motion) {
                int ev_base = 0, err_base = 0, major = 0, minor = 0;
                if (m_query(m_dpy, &ev_base, &err_base, &major, &minor)) {
                    m_x11_ok = true;
                }
            }
        }
        if (!m_x11_ok) {
            // Drop the partial X resources; Wayland may still be usable.
            if (m_lib) { dlclose(m_lib); m_lib = nullptr; }
            if (m_dpy) { XCloseDisplay(m_dpy); m_dpy = nullptr; }
        }
    }

    m_use_wayland = (!m_x11_ok && m_wl_ok);
    m_available = m_x11_ok || m_wl_ok;
    return m_available;
}

void InputForwarder::set_target(unsigned long win) {
    m_win = win;
}

void InputForwarder::focus_target() {
    if (!m_available || !m_win) return;
    XSetInputFocus(m_dpy, static_cast<Window>(m_win), RevertToPointerRoot, CurrentTime);
    XFlush(m_dpy);
}

void InputForwarder::update_geometry(int img_w, int img_h, int disp_w, int disp_h,
                                     double cell_w, double cell_h,
                                     int anchor_col, int anchor_row, int ox, int oy,
                                     int off_x, int off_y,
                                     int vis_w, int vis_h, int src_x, int src_y) {
    m_img_w = img_w;
    m_img_h = img_h;
    m_disp_w = disp_w;
    m_disp_h = disp_h;
    m_vis_w = vis_w > 0 ? vis_w : disp_w;
    m_vis_h = vis_h > 0 ? vis_h : disp_h;
    m_src_x = src_x;
    m_src_y = src_y;
    m_cell_w = cell_w;
    m_cell_h = cell_h;
    m_ax = (anchor_col - 1) * cell_w + off_x;
    m_ay = (anchor_row - 1) * cell_h + off_y;
    m_ox = ox;
    m_oy = oy;
}

void InputForwarder::key_event(KeyCode kc, Bool press) {
    if (kc) m_fake_key(m_dpy, kc, press, 0);
}

void InputForwarder::tap(KeySym ks, KeySym mod) {
    if (!m_available) return;
    if (m_use_wayland) {
        if (m_wl) m_wl->key(ks, true, mod == XK_Control_L, mod == XK_Alt_L, mod == XK_Shift_L);
        return;
    }
    KeyCode k = XKeysymToKeycode(m_dpy, ks);
    if (!k) return;

    KeyCode mod_kc = mod != NoSymbol ? XKeysymToKeycode(m_dpy, mod) : 0;
    if (mod_kc) key_event(mod_kc, True);

    bool need_shift = false;
    if (mod != XK_Shift_L) {
        KeySym lo = XkbKeycodeToKeysym(m_dpy, k, 0, 0);
        KeySym hi = XkbKeycodeToKeysym(m_dpy, k, 0, 1);
        if (ks != lo && ks == hi) need_shift = true;
    }
    KeyCode shift_kc = need_shift ? XKeysymToKeycode(m_dpy, XK_Shift_L) : 0;
    if (shift_kc) key_event(shift_kc, True);

    key_event(k, True);
    key_event(k, False);

    if (shift_kc) key_event(shift_kc, False);
    if (mod_kc) key_event(mod_kc, False);
}

bool InputForwarder::map_pixel(int cx, int cy, int& px, int& py) {
    px = 0;
    py = 0;
    if (!m_available || m_disp_w <= 0 || m_disp_h <= 0) return false;
    if (m_img_w <= 0 || m_img_h <= 0) return false;

    // The image is displayed at disp_w x disp_h terminal pixels, anchored at the
    // cell where the cursor was when we started. Convert the incoming
    // coordinate to a position within that region, then map it onto the image.
    double dpx, dpy;
    if (m_pixel_mouse) {
        if (m_cell_w <= 0.0 || m_cell_h <= 0.0) return false;
        dpx = static_cast<double>(cx - 1) - m_ax;  // terminal pixels
        dpy = static_cast<double>(cy - 1) - m_ay;
    } else {
        if (m_cell_w <= 0.0 || m_cell_h <= 0.0) return false;
        dpx = static_cast<double>(cx - 1) * m_cell_w - m_ax;  // cells -> pixels
        dpy = static_cast<double>(cy - 1) * m_cell_h - m_ay;
    }

    bool inside = (dpx >= 0.0 && dpy >= 0.0 &&
                   dpx < m_vis_w && dpy < m_vis_h);

    double fx = (m_src_x + dpx) / m_disp_w;
    double fy = (m_src_y + dpy) / m_disp_h;
    if (fx < 0.0) fx = 0.0;
    if (fy < 0.0) fy = 0.0;
    if (fx > 1.0) fx = 1.0;
    if (fy > 1.0) fy = 1.0;

    px = static_cast<int>(fx * m_img_w);
    py = static_cast<int>(fy * m_img_h);
    if (px >= m_img_w) px = m_img_w - 1;
    if (py >= m_img_h) py = m_img_h - 1;
    return inside;
}

void InputForwarder::send_motion(int px, int py) {
    if (m_use_wayland) {
        if (m_wl) m_wl->pointer_motion(m_ox + px, m_oy + py);
        return;
    }
    m_fake_motion(m_dpy, DefaultScreen(m_dpy), m_ox + px, m_oy + py, 0);
}

void InputForwarder::button_event(int xbutton, Bool press) {
    if (!m_available) return;
    if (m_use_wayland) {
        if (m_wl) m_wl->button(xbutton, press);
        return;
    }
    m_fake_button(m_dpy, static_cast<unsigned int>(xbutton), press, 0);
}

void InputForwarder::handle_mouse(int cb, int cx, int cy, char term) {
    // Remember the cursor position (as terminal cells) so the caller can route
    // input to the window under the mouse.
    if (m_pixel_mouse && m_cell_w > 0.0 && m_cell_h > 0.0) {
        m_last_col = static_cast<int>((cx - 1) / m_cell_w) + 1;
        m_last_row = static_cast<int>((cy - 1) / m_cell_h) + 1;
    } else {
        m_last_col = cx;
        m_last_row = cy;
    }
    m_have_mouse = true;

    if (m_mouse_hook && m_mouse_hook(cb, cx, cy, term)) {
        return;  // consumed (e.g. by a resize handle)
    }

    bool wheel = (cb & 64) != 0;
    int btn = cb & 3;

    bool any_pressed = m_pressed[0] || m_pressed[1] || m_pressed[2];
    int px = 0, py = 0;
    bool inside = map_pixel(cx, cy, px, py);

    // Releases are always forwarded (with clamped coordinates) so a button
    // never gets stuck when the pointer leaves the image mid-drag.
    if (term == 'm') {
        send_motion(px, py);
        button_event(btn + 1, False);
        if (btn < 8) m_pressed[btn] = false;
        return;
    }

    // Outside the rendered region, ignore new presses/hover/scroll unless a
    // drag is already in progress.
    if (!inside && !any_pressed) return;

    send_motion(px, py);

    if (wheel) {
        if (m_use_wayland) {  // 64 = up, 65 = down
            if (m_wl) m_wl->wheel((cb & 1) ? -1 : 1);
            return;
        }
        int xbtn = (cb & 1) ? 5 : 4;
        button_event(xbtn, True);
        button_event(xbtn, False);
        return;
    }
    // 'M': only press a button if it isn't already down (motion events report
    // the held button repeatedly, and btn==3 means no button / pure motion).
    if (btn < 3 && !m_pressed[btn]) {
        button_event(btn + 1, True);
        m_pressed[btn] = true;
    }
}

KeySym InputForwarder::decode_char(const char* p, size_t n, size_t& adv) {
    unsigned char c = static_cast<unsigned char>(p[0]);
    if (c < 0x80) {
        adv = 1;
        return c;
    }
    int len = 0;
    unsigned int cp = 0;
    if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
    else { adv = 1; return NoSymbol; }

    if (n < static_cast<size_t>(len)) {
        adv = 0;  // incomplete, wait for more bytes
        return NoSymbol;
    }
    for (int k = 1; k < len; ++k) {
        if ((static_cast<unsigned char>(p[k]) & 0xC0) != 0x80) {
            adv = 1;
            return NoSymbol;
        }
        cp = (cp << 6) | (static_cast<unsigned char>(p[k]) & 0x3F);
    }
    adv = len;
    if (cp <= 0xFF) return static_cast<KeySym>(cp);
    KeySym ks = 0x01000000 | cp;
    if (m_dpy && XKeysymToKeycode(m_dpy, ks) == 0) return NoSymbol;
    return ks;
}

void InputForwarder::parse() {
    size_t i = 0;
    while (i < m_pending.size()) {
        unsigned char c = static_cast<unsigned char>(m_pending[i]);

        if (c == 0x1b) {
            // SGR mouse event: ESC [ < ...
            if (i + 2 < m_pending.size() && m_pending[i + 1] == '[' &&
                m_pending[i + 2] == '<') {
                size_t j = i + 3;
                char term = 0;
                while (j < m_pending.size()) {
                    char ch = m_pending[j];
                    if (ch == 'M' || ch == 'm') { term = ch; break; }
                    if ((ch < '0' || ch > '9') && ch != ';') break;
                    ++j;
                }
                if (!term) break;  // incomplete
                size_t k = i + 3;
                int cb = 0, cx = 0, cy = 0;
                bool ok = parse_uint(m_pending, k, cb) && k < j && m_pending[k] == ';' && ++k;
                ok = ok && parse_uint(m_pending, k, cx);
                ok = ok && k < j && m_pending[k] == ';' && ++k;
                ok = ok && parse_uint(m_pending, k, cy);
                if (ok) handle_mouse(cb, cx, cy, term);
                i = j + 1;
                continue;
            }

            // Known sequences (longest match first is not needed: all distinct).
            bool matched = false;
            for (const auto& e : kSeqs) {
                size_t L = std::strlen(e.seq);
                if (m_pending.compare(i, L, e.seq) == 0) {
                    tap(e.ks);
                    i += L;
                    matched = true;
                    break;
                }
            }
            if (matched) continue;

            // Unknown CSI / SS3 sequence: skip to the final byte.
            if (i + 1 < m_pending.size() &&
                (m_pending[i + 1] == '[' || m_pending[i + 1] == 'O')) {
                size_t j = i + 2;
                while (j < m_pending.size()) {
                    unsigned char f = static_cast<unsigned char>(m_pending[j]);
                    if (f >= 0x40 && f <= 0x7e) break;
                    ++j;
                }
                if (j >= m_pending.size()) break;  // incomplete
                i = j + 1;
                continue;
            }

            // Lone ESC => Escape.
            if (i + 1 >= m_pending.size()) {
                tap(XK_Escape);
                i += 1;
                continue;
            }
            // ESC + char => Alt+char.
            size_t adv = 0;
            KeySym ks = decode_char(m_pending.data() + i + 1,
                                    m_pending.size() - (i + 1), adv);
            if (adv == 0) break;
            if (ks != NoSymbol) tap(ks, XK_Alt_L);
            i += 1 + adv;
            continue;
        }

        if (c == 0x0d) { tap(XK_Return); i += 1; continue; }
        if (c == 0x7f) { tap(XK_BackSpace); i += 1; continue; }
        if (c == 0x09) { tap(XK_Tab); i += 1; continue; }
        if (c == 0x00) { tap(XK_space, XK_Control_L); i += 1; continue; }
        if (c >= 0x01 && c <= 0x1a) {
            KeySym ks = XK_a + (c - 1);
            tap(ks, XK_Control_L);
            i += 1;
            continue;
        }

        size_t adv = 0;
        KeySym ks = decode_char(m_pending.data() + i, m_pending.size() - i, adv);
        if (adv == 0) break;  // incomplete
        if (ks != NoSymbol) tap(ks);
        i += (adv ? adv : 1);
    }
    m_pending.erase(0, i);
}

void InputForwarder::feed(const char* data, size_t n) {
    if (!m_available || n == 0) return;
    m_pending.append(data, n);
    parse();
    if (m_dpy) XFlush(m_dpy);
}

bool InputForwarder::last_mouse(int& col, int& row) const {
    if (!m_have_mouse) return false;
    col = m_last_col;
    row = m_last_row;
    return true;
}
