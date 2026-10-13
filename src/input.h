#pragma once

#include <X11/Xlib.h>
#include <functional>
#include <string>

class WaylandServer;

// Translates terminal input (keyboard escape sequences and SGR mouse events)
// into synthetic X events on the virtual display via the XTEST extension.
class InputForwarder {
public:
    explicit InputForwarder(const std::string& display_name);
    ~InputForwarder();

    bool init();                 // open display, load XTEST, query support
    void close();                // release the X connection (idempotent)
    bool available() const { return m_available; }

    // Offer a Wayland compositor as a possible input target (in addition to
    // XTEST). Which one is actually used is chosen at runtime with
    // set_wayland_active().
    void set_wayland(WaylandServer* wl) { m_wl = wl; }

    // Route input to the Wayland compositor (true) or to XTEST (false). Only
    // takes effect for a backend that was available at init().
    void set_wayland_active(bool on) { m_use_wayland = on; }

    void set_target(unsigned long win);
    void focus_target();

    // Geometry of the currently displayed image, used to map terminal cells
    // (or terminal pixels) to screen pixels for mouse input.
    //   img_w/img_h  : source (app) pixel size
    //   disp_w/disp_h: displayed pixel size in the terminal
    //   cell_w/cell_h: terminal cell size in pixels
    //   anchor_col/row: 1-based terminal cell where the (visible) image starts
    //   ox/oy        : window top-left in root-window coordinates
    //   off_x/off_y  : pixel offset of the image within the anchor cell
    //   vis_w/vis_h  : visible displayed size (<= disp; for edge clipping)
    //   src_x/src_y   : crop origin within the displayed image, in pixels
    void update_geometry(int img_w, int img_h, int disp_w, int disp_h,
                         double cell_w, double cell_h,
                         int anchor_col, int anchor_row, int ox, int oy,
                         int off_x = 0, int off_y = 0,
                         int vis_w = 0, int vis_h = 0,
                         int src_x = 0, int src_y = 0);

    // When true, incoming mouse coordinates are pixels (SGR-Pixels / 1016).
    void set_pixel_mouse(bool enabled) { m_pixel_mouse = enabled; }

    // Feed raw bytes read from the terminal.
    void feed(const char* data, size_t n);

    // Optional hook that sees every mouse event before it is forwarded, with
    // the raw SGR coordinates (pixels when SGR-Pixels is active, else cells).
    // Return true to consume the event (e.g. to drive a resize handle). cb is
    // the SGR button code; term is 'M' (press/motion) or 'm' (release).
    using MouseHook = std::function<bool(int cb, int cx, int cy, char term)>;
    void set_mouse_hook(MouseHook h) { m_mouse_hook = std::move(h); }

    // Last mouse position seen, in 1-based terminal cells. Returns false until a
    // mouse event has been received.
    bool last_mouse(int& col, int& row) const;

private:
    typedef Bool (*PFN_Query)(Display*, int*, int*, int*, int*);
    typedef Bool (*PFN_Key)(Display*, unsigned int, Bool, unsigned long);
    typedef Bool (*PFN_Button)(Display*, unsigned int, Bool, unsigned long);
    typedef Bool (*PFN_Motion)(Display*, int, int, int, unsigned long);

    void parse();
    void tap(KeySym ks, KeySym mod = NoSymbol);
    void key_event(KeyCode kc, Bool press);
    // Maps a mouse position to an image pixel (clamped); returns false if the
    // position lies outside the rendered image region.
    bool map_pixel(int cx, int cy, int& px, int& py);
    void send_motion(int px, int py);
    void button_event(int xbutton, Bool press);
    void handle_mouse(int cb, int cx, int cy, char term);
    KeySym decode_char(const char* p, size_t n, size_t& adv);

    std::string m_display_name;
    Display* m_dpy = nullptr;
    unsigned long m_win = 0;
    bool m_available = false;
    bool m_x11_ok = false;
    bool m_wl_ok = false;
    bool m_use_wayland = false;
    WaylandServer* m_wl = nullptr;

    void* m_lib = nullptr;
    PFN_Query m_query = nullptr;
    PFN_Key m_fake_key = nullptr;
    PFN_Button m_fake_button = nullptr;
    PFN_Motion m_fake_motion = nullptr;

    std::string m_pending;

    int m_img_w = 0;
    int m_img_h = 0;
    int m_disp_w = 0;
    int m_disp_h = 0;
    int m_vis_w = 0;
    int m_vis_h = 0;
    int m_src_x = 0;
    int m_src_y = 0;
    double m_cell_w = 0.0;
    double m_cell_h = 0.0;
    double m_ax = 0.0;  // image origin in terminal pixels
    double m_ay = 0.0;
    int m_ox = 0;
    int m_oy = 0;
    bool m_pixel_mouse = false;

    int m_last_col = 0;
    int m_last_row = 0;
    bool m_have_mouse = false;
    MouseHook m_mouse_hook;

    bool m_pressed[8] = {false, false, false, false, false, false, false, false};
};
