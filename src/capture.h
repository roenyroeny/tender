#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct Frame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgb;  // 3 bytes per pixel, RGB order
};

// Bilinear-resize src into dst (dst sized w x h).
void resize_frame(const Frame& src, Frame& dst, int w, int h);

class Capturer {
public:
    explicit Capturer(const std::string& display_name);
    ~Capturer();

    // Returns true and fills frame on success. If out_x/out_y are non-null they
    // receive the captured window's top-left position in root coordinates.
    bool capture(unsigned long win_id, Frame& frame, int* out_x = nullptr, int* out_y = nullptr);

    // All currently-viewable top-level windows of a reasonable size, in a stable
    // order (by window id). Reuses the open X connection, so it is cheap enough
    // to poll every frame.
    void discover_windows(std::vector<unsigned long>& out);

    // Title of a window (prefers _NET_WM_NAME, falls back to WM_NAME).
    std::string window_title(unsigned long win_id);

    // Resize a window on the virtual display.
    bool resize_window(unsigned long win_id, int w, int h);

    // Whether the window advertises that it can be resized (i.e. its size hints
    // do not pin both dimensions).
    bool window_resizable(unsigned long win_id);

    // Whether the window supports WM_DELETE_WINDOW (can be asked to close).
    bool window_can_close(unsigned long win_id);

    // Ask the window to close (sends the WM_DELETE_WINDOW client message).
    void close_window(unsigned long win_id);

    // Root-window coordinates of the last captured frame's top-left corner.
    int last_x() const { return m_last_x; }
    int last_y() const { return m_last_y; }

private:
    std::string m_display_name;
    void* m_display = nullptr;  // Display*
    int m_last_x = 0;
    int m_last_y = 0;
    bool m_atoms_ready = false;
    unsigned long m_net_name_atom = 0;
    unsigned long m_utf8_atom = 0;
    bool m_wm_atoms_ready = false;
    unsigned long m_wm_protocols_atom = 0;
    unsigned long m_wm_delete_atom = 0;
};
