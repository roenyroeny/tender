#include "capture.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int ignore_x_error(Display* dpy, XErrorEvent* ev) {
    static int count = 0;
    if (count++ < 5) {
        char buf[256];
        XGetErrorText(dpy, ev->error_code, buf, sizeof(buf));
        std::fprintf(stderr, "tender: X error: %s (request %d.%d)\n",
                     buf, ev->request_code, ev->minor_code);
    }
    return 0;
}

}  // namespace

void resize_frame(const Frame& src, Frame& dst, int w, int h) {
    dst.width = w;
    dst.height = h;
    dst.rgb.assign(static_cast<size_t>(w) * h * 3, 0);
    if (src.width <= 0 || src.height <= 0 || w <= 0 || h <= 0) return;

    for (int y = 0; y < h; ++y) {
        double sy = (y + 0.5) * src.height / h - 0.5;
        int y0 = static_cast<int>(std::floor(sy));
        double fy = sy - y0;
        if (sy < 0) { y0 = 0; fy = 0; }
        int y1 = y0 + 1;
        if (y0 >= src.height) y0 = src.height - 1;
        if (y1 >= src.height) y1 = src.height - 1;

        for (int x = 0; x < w; ++x) {
            double sx = (x + 0.5) * src.width / w - 0.5;
            int x0 = static_cast<int>(std::floor(sx));
            double fx = sx - x0;
            if (sx < 0) { x0 = 0; fx = 0; }
            int x1 = x0 + 1;
            if (x0 >= src.width) x0 = src.width - 1;
            if (x1 >= src.width) x1 = src.width - 1;

            const uint8_t* p00 = &src.rgb[(static_cast<size_t>(y0) * src.width + x0) * 3];
            const uint8_t* p01 = &src.rgb[(static_cast<size_t>(y0) * src.width + x1) * 3];
            const uint8_t* p10 = &src.rgb[(static_cast<size_t>(y1) * src.width + x0) * 3];
            const uint8_t* p11 = &src.rgb[(static_cast<size_t>(y1) * src.width + x1) * 3];
            uint8_t* o = &dst.rgb[(static_cast<size_t>(y) * w + x) * 3];
            for (int c = 0; c < 3; ++c) {
                double top = p00[c] + (p01[c] - p00[c]) * fx;
                double bot = p10[c] + (p11[c] - p10[c]) * fx;
                double v = top + (bot - top) * fy;
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                o[c] = static_cast<uint8_t>(v + 0.5);
            }
        }
    }
}

Capturer::Capturer(const std::string& display_name)
    : m_display_name(display_name) {
    setenv("DISPLAY", m_display_name.c_str(), 1);
    // XGetImage can legitimately fail (e.g. non-viewable drawable); don't let
    // the default handler abort the process.
    XSetErrorHandler(ignore_x_error);
    m_display = XOpenDisplay(nullptr);
}

Capturer::~Capturer() {
    if (m_display) {
        XCloseDisplay(static_cast<Display*>(m_display));
    }
}

void Capturer::discover_windows(std::vector<unsigned long>& out) {
    out.clear();
    Display* dpy = static_cast<Display*>(m_display);
    if (!dpy) {
        return;
    }
    Window root = DefaultRootWindow(dpy);
    Window root_r, parent_r;
    Window* children = nullptr;
    unsigned int nchildren = 0;
    if (XQueryTree(dpy, root, &root_r, &parent_r, &children, &nchildren) == 0) {
        if (children) XFree(children);
        return;
    }

    for (unsigned int i = 0; i < nchildren; ++i) {
        XWindowAttributes attrs;
        if (XGetWindowAttributes(dpy, children[i], &attrs) != 0) {
            // Require a reasonably sized viewable window; tiny helper/override
            // windows are ignored.
            if (attrs.map_state == IsViewable && attrs.width > 1 && attrs.height > 1 &&
                attrs.width * attrs.height >= 100) {
                out.push_back(static_cast<unsigned long>(children[i]));
            }
        }
    }
    if (children) XFree(children);
    // Stable order so tiles don't shuffle between frames.
    std::sort(out.begin(), out.end());
}

std::string Capturer::window_title(unsigned long win_id) {
    Display* dpy = static_cast<Display*>(m_display);
    if (!dpy) return {};
    Window win = static_cast<Window>(win_id);
    std::string title;

    // Prefer the UTF-8 _NET_WM_NAME property.
    if (!m_atoms_ready) {
        m_net_name_atom = XInternAtom(dpy, "_NET_WM_NAME", True);
        m_utf8_atom = XInternAtom(dpy, "UTF8_STRING", True);
        m_atoms_ready = true;
    }
    Atom net_name = m_net_name_atom;
    Atom utf8 = m_utf8_atom;
    if (net_name != None && utf8 != None) {
        Atom type = None;
        int format = 0;
        unsigned long nitems = 0, after = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(dpy, win, net_name, 0, 1024, False, utf8, &type,
                               &format, &nitems, &after, &data) == Success &&
            data) {
            title.assign(reinterpret_cast<char*>(data), nitems);
            XFree(data);
        }
    }

    if (title.empty()) {
        char* name = nullptr;
        if (XFetchName(dpy, win, &name) && name) {
            title = name;
            XFree(name);
        }
    }
    return title;
}

bool Capturer::window_resizable(unsigned long win_id) {
    Display* dpy = static_cast<Display*>(m_display);
    if (!dpy) return false;
    XSizeHints hints;
    std::memset(&hints, 0, sizeof(hints));
    long supplied = 0;
    if (!XGetWMNormalHints(dpy, static_cast<Window>(win_id), &hints, &supplied)) {
        return true;  // no hints -> assume resizable
    }
    bool fixed_w = (supplied & PMinSize) && (supplied & PMaxSize) &&
                   hints.min_width > 0 && hints.min_width == hints.max_width;
    bool fixed_h = (supplied & PMinSize) && (supplied & PMaxSize) &&
                   hints.min_height > 0 && hints.min_height == hints.max_height;
    return !(fixed_w && fixed_h);
}

bool Capturer::window_can_close(unsigned long win_id) {
    Display* dpy = static_cast<Display*>(m_display);
    if (!dpy) return false;
    if (!m_wm_atoms_ready) {
        m_wm_protocols_atom = XInternAtom(dpy, "WM_PROTOCOLS", True);
        m_wm_delete_atom = XInternAtom(dpy, "WM_DELETE_WINDOW", True);
        m_wm_atoms_ready = true;
    }
    if (m_wm_protocols_atom == None || m_wm_delete_atom == None) return false;
    Atom* protos = nullptr;
    int count = 0;
    bool can = false;
    if (XGetWMProtocols(dpy, static_cast<Window>(win_id), &protos, &count)) {
        for (int i = 0; i < count; ++i) {
            if (protos[i] == m_wm_delete_atom) can = true;
        }
        if (protos) XFree(protos);
    }
    return can;
}

void Capturer::close_window(unsigned long win_id) {
    Display* dpy = static_cast<Display*>(m_display);
    if (!dpy) return;
    if (!m_wm_atoms_ready) {
        m_wm_protocols_atom = XInternAtom(dpy, "WM_PROTOCOLS", True);
        m_wm_delete_atom = XInternAtom(dpy, "WM_DELETE_WINDOW", True);
        m_wm_atoms_ready = true;
    }
    XEvent ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = static_cast<Window>(win_id);
    ev.xclient.message_type = m_wm_protocols_atom;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = static_cast<long>(m_wm_delete_atom);
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, static_cast<Window>(win_id), False, NoEventMask, &ev);
    XFlush(dpy);
}

bool Capturer::resize_window(unsigned long win_id, int w, int h) {
    Display* dpy = static_cast<Display*>(m_display);
    if (!dpy || w <= 0 || h <= 0) return false;
    XResizeWindow(dpy, static_cast<Window>(win_id),
                  static_cast<unsigned int>(w), static_cast<unsigned int>(h));
    XFlush(dpy);
    return true;
}

bool Capturer::capture(unsigned long win_id, Frame& frame, int* out_x, int* out_y) {
    Display* dpy = static_cast<Display*>(m_display);
    if (!dpy) {
        return false;
    }
    Window win = static_cast<Window>(win_id);

    XWindowAttributes attrs;
    if (!XGetWindowAttributes(dpy, win, &attrs)) {
        return false;
    }
    int w = attrs.width;
    int h = attrs.height;
    if (w <= 0 || h <= 0) {
        return false;
    }

    // Capture from the root window, cropped to the target window's geometry.
    // Reading the root (instead of the window itself) composites child windows,
    // which is required for most toolkits that draw into sub-windows.
    Window root = DefaultRootWindow(dpy);
    const int root_w = DisplayWidth(dpy, DefaultScreen(dpy));
    const int root_h = DisplayHeight(dpy, DefaultScreen(dpy));

    int rx = attrs.x;
    int ry = attrs.y;
    {
        Window child_ret;
        int tx = 0, ty = 0;
        if (XTranslateCoordinates(dpy, win, root, 0, 0, &tx, &ty, &child_ret)) {
            rx = tx;
            ry = ty;
        }
    }

    // Clamp the crop rectangle to the root bounds; a window border can push the
    // rectangle past the edge, which makes XGetImage raise BadMatch.
    if (rx < 0) {
        w += rx;
        rx = 0;
    }
    if (ry < 0) {
        h += ry;
        ry = 0;
    }
    if (rx + w > root_w) w = root_w - rx;
    if (ry + h > root_h) h = root_h - ry;
    if (w <= 0 || h <= 0) {
        return false;
    }

    m_last_x = rx;
    m_last_y = ry;
    if (out_x) *out_x = rx;
    if (out_y) *out_y = ry;

    XImage* img = XGetImage(dpy, root, rx, ry, w, h, AllPlanes, ZPixmap);
    if (!img) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                         "tender: root capture failed; win=0x%lx x=%d y=%d "
                         "w=%d h=%d border=%d depth=%d map=%d; root=%dx%d "
                         "crop=(%d,%d %dx%d)\n",
                         static_cast<unsigned long>(win), attrs.x, attrs.y,
                         attrs.width, attrs.height, attrs.border_width,
                         attrs.depth, static_cast<int>(attrs.map_state),
                         root_w, root_h, rx, ry, w, h);
        }
        // Fallback: read the window drawable directly.
        w = attrs.width;
        h = attrs.height;
        img = XGetImage(dpy, win, 0, 0, w, h, AllPlanes, ZPixmap);
        if (!img) {
            return false;
        }
    }

    frame.width = w;
    frame.height = h;
    frame.rgb.resize(static_cast<size_t>(w) * h * 3);

    // Convert XImage to tightly packed RGB.
    if (img->bits_per_pixel == 32 && img->byte_order == LSBFirst) {
        // Fast path: memory layout B,G,R,X on little endian.
        for (int y = 0; y < h; ++y) {
            const uint8_t* row =
                reinterpret_cast<const uint8_t*>(img->data) +
                static_cast<size_t>(y) * img->bytes_per_line;
            uint8_t* out = frame.rgb.data() + static_cast<size_t>(y) * w * 3;
            for (int x = 0; x < w; ++x) {
                out[x * 3 + 0] = row[x * 4 + 2];  // R
                out[x * 3 + 1] = row[x * 4 + 1];  // G
                out[x * 3 + 2] = row[x * 4 + 0];  // B
            }
        }
    } else {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                unsigned long pixel = XGetPixel(img, x, y);
                uint8_t r = (pixel >> 16) & 0xFF;
                uint8_t g = (pixel >> 8) & 0xFF;
                uint8_t b = pixel & 0xFF;
                size_t idx = (static_cast<size_t>(y) * w + x) * 3;
                frame.rgb[idx + 0] = r;
                frame.rgb[idx + 1] = g;
                frame.rgb[idx + 2] = b;
            }
        }
    }

    XDestroyImage(img);
    return true;
}
