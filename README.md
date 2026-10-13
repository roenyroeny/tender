# tender

Run a GUI program on private virtual displays (X11 via Xvfb and a built-in
headless Wayland compositor, both at once) and render its graphical output into
the terminal using the Kitty graphics protocol, falling back to sixel when the
terminal does not support Kitty. No real window is created: the program picks
whichever backend its toolkit prefers, and windows from either are detected
automatically.

## Usage

```bash
tender [OPTIONS] -- <program> [args...]
```

Examples:
```bash
tender xeyes
```

Options:
| Option | Description |
| --- | --- |
| `--fps N` | Target frames per second (default 30) |
| `--size WxH` | Max virtual screen size (default 1920x1080). The program keeps its own window resolution; this only bounds the virtual desktop. |
| `--display N` | Display number for the Xvfb server to use |
| `--connect :N` | Use an already running X server (spawns no server) |
| `--scale F` | Scale the image within the terminal (default 1.0) |
| `-f`, `--fullscreen` | Use the alternate screen so the whole terminal is the workspace |
| `--sixel` | Force the sixel renderer instead of auto-detecting Kitty |
| `--ansi` | Force the low-resolution ANSI half-block fallback renderer |

## Placement

With `-f`/`--fullscreen` tender switches to the alternate screen, clears it,
and uses the whole terminal as a workspace (leaving restores your previous
content); otherwise it draws inline at the current cursor position. Each window
is surrounded by a rounded box (`╭─╮` / `│` / `╰─╯`, or `+`/`-`/`|` when the
locale is not UTF-8) with the window's title in the top edge (`╭─ title ─╮`).
Each image sits one cell inside its border. Frames are shown at the program's
own pixel size (or scaled with `--scale`), and mouse coordinates are mapped
within that exact region.

## Multiple windows

Every window the program creates is displayed at once, tiled in a grid (one
kitty image per window). X11 top-levels and Wayland toplevels are both included,
so a single session can show windows from either backend (or both). Tiles are
added as new windows appear and removed when they close; the grid is recomputed
whenever the window set or terminal size changes. Mouse input is routed to the
window under the terminal cursor, and keyboard focus follows it (X11).

Resizable windows get a handle at their bottom-right corner (`◢`): drag it to
resize the underlying window (X11 `XResizeWindow`; Wayland via an xdg-toplevel
configure). Windows that pin their size (X11 `WM_NORMAL_HINTS` min == max, or
Wayland min/max size) don't get a handle and can't be resized. Windows that
support closing get an `✕` at their top-right, which sends `WM_DELETE_WINDOW`
(X11) or `xdg_toplevel.close` (Wayland). Drag a window's **titlebar** to move
it: it leaves the auto grid and floats at the dropped position (windows may
overlap). When the grid is taller than the terminal, scroll the mouse wheel
**over a window's border** to pan the viewport (wheel over the content still
goes to the app). A window may be made taller/wider than the terminal: the grid pins
its bottom-right (keeping the handle visible) and the overflowing top/left is
clipped; holding the handle against the bottom or right edge keeps growing it.

## Input

Keyboard and mouse input are forwarded to the program (XTEST for X11, `wl_seat`
for Wayland):

- Printable characters, Ctrl+letter, Alt+char, arrows, Home/End, PgUp/PgDn,
  Insert/Delete, Tab, Enter, Backspace, Escape and F1-F12.
- SGR mouse: move/hover (no button needed), click, drag, scroll wheel.
- **Pixel-perfect mouse** when the terminal supports SGR-Pixels (DECSET 1016,
  e.g. kitty/xterm): detected via `DECRQM`, coordinates are used 1:1. Otherwise
  terminal cells are mapped to screen pixels using the image geometry.

Mouse reporting captures the mouse, so hold **Shift** for normal terminal
text selection. If the X server lacks XTEST, tender falls back to
display-only and says so.

## Signals

- **Ctrl+C / SIGINT/SIGTERM/SIGHUP/SIGQUIT** — stops rendering, deletes the
  image, and restores the cursor/terminal state before exiting.
- **Ctrl+Z** — hands the terminal back and suspends; `fg` re-acquires it.

The terminal is also restored via an `atexit` hook as a safety net.

## Wayland

Alongside Xvfb, tender runs a small built-in headless compositor and
captures its buffers. No external compositor is needed, and both backends are
active at the same time: the program picks whichever its toolkit prefers.
It implements `wl_compositor`, `wl_shm`, `xdg-shell`, `wl_subcompositor`
(subsurfaces are composited over their parent), `wl_seat` (xkbcommon keymap) and
`zwp_linux_dmabuf_v1` (imported via EGL and read back). Keyboard and pointer
input are delivered through the seat or via XTEST, depending on which window is
currently displayed.

This is enough to run real toolkits and browsers:

```bash
tender -- zenity --info --text=hello
tender --size 1000x750 -- firefox --no-remote --new-instance about:blank
```

Notes:
- GPU/EGL buffers are imported through the DRM render node and read back; if EGL
  init fails, the backend still works for `wl_shm` clients.
- Some clients render their content into a **subsurface** (e.g. Firefox); these
  are composited over the toplevel.
- If the built-in compositor is unavailable (missing Wayland dev libraries), or
  Xvfb is missing, tender runs with whichever backend is left.

## Requirements

- An X server backend: `Xvfb` (or use `--connect` for an existing display).
  Install with e.g. `sudo apt install xvfb`.
- Dev libraries: `libX11`, `libXext`, `libXfixes`, `zlib`.
- Optional (Wayland backend): `wayland-server`, `wayland-protocols`,
  `wayland-scanner`, `xkbcommon`, and `egl`/`glesv2` (for dmabuf clients).
  Without these, tender builds and runs X11-only.
- A terminal for display. The protocol is auto-detected: Kitty-compatible
  (Kitty, Ghostty, WezTerm, Konsole, ...), else sixel (xterm with sixel, foot,
  mlterm, ...). If the terminal supports neither, tender exits with an
  error unless `--ansi` is given, which selects a low-resolution 24-bit-colour
  half-block fallback that works on any truecolor terminal (e.g. GNOME Terminal).

## Building

```bash
cmake -B build
cmake --build build
./build/tender xeyes
```
