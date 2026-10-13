#pragma once

#include <cstddef>
#include <string>

struct TermSize {
    int cols = 80;
    int rows = 24;
    double cell_w = 0.0;   // pixels per cell width (0 = unknown)
    double cell_h = 0.0;   // pixels per cell height (0 = unknown)
};

class Terminal {
public:
    // Bitmap graphics protocol the terminal understands. `Ansi` is the
    // universal fallback (24-bit colour half-blocks); `None` means neither the
    // Kitty protocol nor sixel is available (use --ansi to force the fallback).
    enum class Graphics { Unsupported, Kitty, Sixel, Ansi };

    Terminal();
    ~Terminal();

    bool init();          // raw mode + alternate screen + hide cursor
    void restore();       // undo init() and forget saved settings
    void suspend();       // hand the terminal back (e.g. on Ctrl+Z)
    void resume();        // re-acquire it after suspend()

    // Enable SGR mouse reporting (call before init()).
    void set_mouse(bool enabled) { m_mouse = enabled; }

    // When true, init() uses the alternate screen (clearing it) so the whole
    // terminal is a workspace; restore() returns to the previous screen.
    void set_fullscreen(bool on) { m_fullscreen = on; }

    // Ask the terminal for the current cursor position (1-based). Returns
    // false (and leaves defaults) if the terminal does not report it.
    bool query_cursor_position(int& row, int& col);

    // Ask the terminal whether it supports SGR-Pixels (DECSET 1016), i.e.
    // mouse coordinates reported in pixels instead of cells.
    bool probe_pixel_mouse();
    void set_pixel_mouse(bool enabled) { m_pixel_mouse = enabled; }
    bool pixel_mouse() const { return m_pixel_mouse; }

    // Query the terminal's default background colour (OSC 11). Returns false
    // and leaves black on failure.
    bool query_background(int& r, int& g, int& b);

    // Detect which bitmap graphics protocol the terminal supports. Prefers the
    // Kitty protocol and falls back to sixel. Defaults to Kitty when the
    // terminal does not answer either probe.
    Graphics detect_graphics();
    Graphics graphics() const { return m_graphics; }

    int out_fd() const;   // fd to write graphics/escape sequences
    int in_fd() const;    // fd to read user input

    TermSize size() const;                 // uses ioctl, falls back to defaults
    bool poll_input(char* buf, size_t n, int timeout_ms, size_t& got);

    bool is_active() const { return m_initialized; }

private:
    void apply_raw();
    void revert();

    int m_out_fd;
    int m_in_fd;
    bool m_initialized;
    bool m_mouse = false;
    bool m_pixel_mouse = false;
    bool m_fullscreen = false;
    Graphics m_graphics = Graphics::Unsupported;
    bool m_have_termios;
    void* m_saved_termios;  // struct termios*
};
