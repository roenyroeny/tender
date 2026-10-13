#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "capture.h"

// A minimal headless Wayland compositor that hosts a single xdg-shell toplevel
// rendered through wl_shm, and captures it into a Frame. Enough to run
// software-rendered Wayland clients without a real compositor.
class WaylandServer {
public:
    WaylandServer(int width, int height);
    ~WaylandServer();

    bool start();   // create display, socket and globals
    void stop();

    std::string runtime_dir() const { return m_runtime_dir; }
    std::string socket_name() const { return m_socket_name; }

    // Process pending protocol events. Returns false on a fatal error.
    bool poll(int timeout_ms);

    // Copy the latest committed toplevel buffer into `out`. Returns false if
    // nothing has been committed yet.
    bool capture(Frame& out);

    // IDs of all current xdg toplevels (stable for as long as the surface
    // lives), in a stable order.
    void toplevels(std::vector<unsigned long>& out) const;

    // Title set by a toplevel (empty if none/unset).
    std::string toplevel_title(unsigned long id) const;

    // Ask a toplevel to resize to w x h pixels (via xdg configure).
    bool resize_toplevel(unsigned long id, int w, int h);

    // Whether a toplevel allows resizing (its min/max size hints don't pin it).
    bool toplevel_resizable(unsigned long id) const;

    // Whether a toplevel can be asked to close.
    bool toplevel_can_close(unsigned long id) const;

    // Ask a toplevel to close (sends the xdg_toplevel.close event).
    void close_toplevel(unsigned long id);

    // Copy the composited frame of a specific toplevel (by ID from toplevels()).
    // Returns false if it has no committed content.
    bool capture_toplevel(unsigned long id, Frame& out);

    bool has_toplevel() const;

    // --- input injection ---
    bool input_available() const;
    void key(unsigned int keysym, bool press, bool ctrl, bool alt, bool shift);
    void pointer_motion(int x, int y);
    void button(int xbutton, bool press);
    void wheel(int dir);   // +1 up, -1 down

private:
    int m_width;
    int m_height;
    std::string m_runtime_dir;
    std::string m_socket_name;
    void* m_display = nullptr;   // struct wl_display*
    void* m_event_loop = nullptr;
    void* m_impl = nullptr;      // Impl*
};
