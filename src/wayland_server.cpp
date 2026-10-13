#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "wayland_server.h"

#if HAVE_WAYLAND

extern "C" {
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "xdg-shell-server-protocol.h"
}

#include <xkbcommon/xkbcommon.h>

#if HAVE_DMABUF
extern "C" {
#include "linux-dmabuf-server-protocol.h"
}
#include "egl_import.h"
#endif

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <vector>

namespace {

struct Impl;

static uint32_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// Clients (e.g. GTK/GDK) batch pointer events and only dispatch them when a
// wl_pointer.frame arrives, so every pointer event group must be terminated
// with one on protocol version >= 5.
static void pointer_frame(wl_resource* p) {
    if (wl_resource_get_version(p) >= WL_POINTER_FRAME_SINCE_VERSION) {
        wl_pointer_send_frame(p);
    }
}

struct ShmPool {
    wl_resource* res = nullptr;
    int fd = -1;
    void* data = nullptr;
    size_t size = 0;
    int refs = 0;
    bool destroyed = false;
};

struct DmaBufPlaneStore {
    int fd = -1;
    uint32_t offset = 0;
    uint32_t stride = 0;
};

// A wl_buffer is either backed by wl_shm or by linux-dmabuf.
struct Buffer {
    bool is_dmabuf = false;
    // shm
    ShmPool* pool = nullptr;
    int32_t offset = 0;
    uint32_t stride = 0;
    // common
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    // dmabuf
    uint64_t modifier = 0;
    int nplanes = 0;
    DmaBufPlaneStore planes[4];
};

struct Surface {
    Impl* impl = nullptr;
    wl_resource* res = nullptr;
    wl_resource* pending_buffer = nullptr;
    wl_resource* current_buffer = nullptr;
    wl_resource* xdg_surface = nullptr;
    wl_resource* toplevel = nullptr;
    bool configured = false;
    bool configure_sent = false;
    wl_resource* frame_cb = nullptr;
    int win_w = 0;
    int win_h = 0;
    bool is_toplevel = false;
    bool focused = false;
    bool maximized = false;
    int min_w = 0, min_h = 0, max_w = 0, max_h = 0;
    std::string title;

    // Subsurface / popup support (both are composited over a parent surface).
    wl_resource* sub_parent = nullptr;  // parent wl_surface resource
    int sub_x = 0;
    int sub_y = 0;
    bool is_subsurface = false;
    bool is_popup = false;
    wl_resource* popup = nullptr;

    // Last committed image for this surface (used for compositing).
    Frame cached;
    bool has_cached = false;
};

struct SeatRes {
    Impl* impl = nullptr;
    uint32_t version = 1;
};

// A wl_pointer or wl_keyboard resource. `entered` tracks whether the resource
// currently has surface focus, scoped to its own client.
struct InputRes {
    Impl* impl = nullptr;
    bool entered = false;
};

// State captured from an xdg_positioner, used to place popups.
struct PositionerData {
    int ax = 0, ay = 0, aw = 0, ah = 0;  // anchor rect
    int ox = 0, oy = 0;                  // offset
    int w = 0, h = 0;                    // requested size
};

struct Impl {
    WaylandServer* owner = nullptr;
    int width = 0;
    int height = 0;
    wl_display* display = nullptr;
    wl_event_loop* loop = nullptr;

    std::vector<Surface*> surfaces;

    Frame frame;
    bool has_frame = false;

    std::vector<wl_resource*> keyboards;
    std::vector<wl_resource*> pointers;

    xkb_context* xkb_ctx = nullptr;
    xkb_keymap* xkb_map = nullptr;
    xkb_state* xkb_st = nullptr;
    uint32_t last_frame_ms = 0;

#if HAVE_DMABUF
    EglImporter* importer = nullptr;
#endif
};

// ---- helpers ---------------------------------------------------------------

static void send_keyboard_focus(Impl*, InputRes*, wl_resource*, wl_resource*);
static void send_pointer_focus(Impl*, InputRes*, wl_resource*, wl_resource*);

static bool buffer_to_frame(Impl* impl, Buffer* b, Frame& out) {
    if (!b) return false;
    if (b->is_dmabuf) {
#if HAVE_DMABUF
        if (!impl->importer) return false;
        DmaBufPlane planes[4];
        for (int i = 0; i < b->nplanes; ++i) {
            planes[i].fd = b->planes[i].fd;
            planes[i].offset = b->planes[i].offset;
            planes[i].stride = b->planes[i].stride;
        }
        return impl->importer->import(planes, b->nplanes, b->width, b->height,
                                      b->format, b->modifier, out);
#else
        return false;
#endif
    }
    if (!b->pool || !b->pool->data) return false;
    size_t need = static_cast<size_t>(b->stride) * b->height;
    if (static_cast<size_t>(b->offset) + need > b->pool->size) return false;
    uint8_t* base = static_cast<uint8_t*>(b->pool->data) + b->offset;

    out.width = static_cast<int>(b->width);
    out.height = static_cast<int>(b->height);
    out.rgb.resize(static_cast<size_t>(b->width) * b->height * 3);
    for (uint32_t y = 0; y < b->height; ++y) {
        const uint8_t* row = base + static_cast<size_t>(y) * b->stride;
        uint8_t* o = out.rgb.data() + static_cast<size_t>(y) * b->width * 3;
        for (uint32_t x = 0; x < b->width; ++x) {
            o[x * 3 + 0] = row[x * 4 + 2];
            o[x * 3 + 1] = row[x * 4 + 1];
            o[x * 3 + 2] = row[x * 4 + 0];
        }
    }
    return true;
}

static Surface* surface_by_res(Impl* impl, wl_resource* res) {
    for (Surface* s : impl->surfaces) if (s->res == res) return s;
    return nullptr;
}

// Composite the toplevel and its subsurfaces into `out`. Returns true if the
// result has usable content.
static bool recompose(Impl* impl, Surface* top, Frame& out) {
    if (top->has_cached) {
        out = top->cached;
    } else {
        out.width = 0;
        out.height = 0;
        out.rgb.clear();
    }
    for (Surface* s : impl->surfaces) {
        if (s->sub_parent != top->res || !s->has_cached) continue;
        if (out.width <= 0 || out.height <= 0) {
            out.width = s->cached.width;
            out.height = s->cached.height;
            out.rgb.assign(static_cast<size_t>(out.width) * out.height * 3, 0);
        }
        for (int y = 0; y < s->cached.height; ++y) {
            int dy = s->sub_y + y;
            if (dy < 0 || dy >= out.height) continue;
            for (int x = 0; x < s->cached.width; ++x) {
                int dx = s->sub_x + x;
                if (dx < 0 || dx >= out.width) continue;
                const uint8_t* src = s->cached.rgb.data() + (static_cast<size_t>(y) * s->cached.width + x) * 3;
                uint8_t* dst = out.rgb.data() + (static_cast<size_t>(dy) * out.width + dx) * 3;
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
            }
        }
    }
    return out.width > 0 && out.height > 0;
}

// ---- wl_region -------------------------------------------------------------

static void region_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void region_add(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
static void region_subtract(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
static const struct wl_region_interface region_impl = { region_destroy, region_add, region_subtract };

// ---- wl_surface ------------------------------------------------------------

static void surface_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void surface_attach(wl_client*, wl_resource* r, wl_resource* buffer, int32_t, int32_t) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    s->pending_buffer = buffer;
}
static void surface_damage(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
static void surface_frame(wl_client* client, wl_resource* r, uint32_t id) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    wl_resource* cb = wl_resource_create(client, &wl_callback_interface, 1, id);
    if (s->frame_cb) wl_resource_destroy(s->frame_cb);
    s->frame_cb = cb;
}
static void surface_set_opaque_region(wl_client*, wl_resource*, wl_resource*) {}
static void surface_set_input_region(wl_client*, wl_resource*, wl_resource*) {}

static void toplevel_send_configure(Surface* s);

static void surface_commit(wl_client*, wl_resource* r) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    Impl* impl = s->impl;
    if (s->is_toplevel && s->toplevel && !s->configure_sent) {
        s->configure_sent = true;
        toplevel_send_configure(s);
    }
    // Give the toplevel keyboard/pointer focus so apps consider it active.
    // Focus is scoped to the connection that owns the surface.
    if (s->is_toplevel && !s->focused) {
        s->focused = true;
        wl_client* cl = wl_resource_get_client(s->res);
        for (wl_resource* kb : impl->keyboards) {
            InputRes* ir = static_cast<InputRes*>(wl_resource_get_user_data(kb));
            if (wl_resource_get_client(kb) == cl && ir && !ir->entered)
                send_keyboard_focus(impl, ir, kb, s->res);
        }
        for (wl_resource* p : impl->pointers) {
            InputRes* ir = static_cast<InputRes*>(wl_resource_get_user_data(p));
            if (wl_resource_get_client(p) == cl && ir && !ir->entered)
                send_pointer_focus(impl, ir, p, s->res);
        }
    }
    if (s->pending_buffer) {
        s->current_buffer = s->pending_buffer;
        s->pending_buffer = nullptr;
        Buffer* b = static_cast<Buffer*>(wl_resource_get_user_data(s->current_buffer));
        if (buffer_to_frame(impl, b, s->cached)) s->has_cached = true;
        wl_buffer_send_release(s->current_buffer);

        // Composite into the root toplevel's frame.
        Surface* root = s;
        int guard = 0;
        while (root && root->sub_parent && guard++ < 8) {
            Surface* p = surface_by_res(impl, root->sub_parent);
            if (!p) break;
            root = p;
        }
        if (root && root->is_toplevel) {
            if (recompose(impl, root, impl->frame)) impl->has_frame = true;
        }
    }
}
static void surface_set_buffer_transform(wl_client*, wl_resource*, int32_t) {}
static void surface_set_buffer_scale(wl_client*, wl_resource*, int32_t) {}
static void surface_damage_buffer(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
static const struct wl_surface_interface surface_impl = {
    surface_destroy, surface_attach, surface_damage, surface_frame,
    surface_set_opaque_region, surface_set_input_region, surface_commit,
    surface_set_buffer_transform, surface_set_buffer_scale, surface_damage_buffer
};

// ---- wl_compositor ---------------------------------------------------------

static void compositor_create_surface(wl_client* client, wl_resource* r, uint32_t id) {
    Impl* impl = static_cast<Impl*>(wl_resource_get_user_data(r));
    Surface* s = new Surface();
    s->impl = impl;
    s->res = wl_resource_create(client, &wl_surface_interface,
                                wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->res, &surface_impl, s, [](wl_resource* rr) {
        Surface* s = static_cast<Surface*>(wl_resource_get_user_data(rr));
        if (s) {
            Impl* impl = s->impl;
            if (impl) {
                for (auto it = impl->surfaces.begin(); it != impl->surfaces.end(); ++it) {
                    if (*it == s) { impl->surfaces.erase(it); break; }
                }
            }
        }
        delete s;
    });
    impl->surfaces.push_back(s);
}
static void compositor_create_region(wl_client* client, wl_resource*, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &wl_region_interface, 1, id);
    wl_resource_set_implementation(res, &region_impl, nullptr, nullptr);
}
static const struct wl_compositor_interface compositor_impl = {
    compositor_create_surface, compositor_create_region
};

// ---- wl_shm ----------------------------------------------------------------

static void buffer_destroy(wl_client*, wl_resource* r) {
    Buffer* b = static_cast<Buffer*>(wl_resource_get_user_data(r));
    if (!b) return;
    if (b->is_dmabuf) {
        for (int i = 0; i < b->nplanes; ++i) {
            if (b->planes[i].fd >= 0) close(b->planes[i].fd);
        }
    } else if (b->pool) {
        b->pool->refs--;
        if (b->pool->destroyed && b->pool->refs <= 0) {
            if (b->pool->data) munmap(b->pool->data, b->pool->size);
            if (b->pool->fd >= 0) close(b->pool->fd);
            delete b->pool;
        }
    }
    delete b;
}
static const struct wl_buffer_interface buffer_impl = { buffer_destroy };

static void shm_pool_create_buffer(wl_client* client, wl_resource* r, uint32_t id,
                                   int32_t offset, int32_t width, int32_t height,
                                   int32_t stride, uint32_t format) {
    ShmPool* pool = static_cast<ShmPool*>(wl_resource_get_user_data(r));
    Buffer* b = new Buffer();
    b->is_dmabuf = false;
    b->pool = pool;
    b->offset = offset;
    b->width = width;
    b->height = height;
    b->stride = stride;
    b->format = format;
    wl_resource* res = wl_resource_create(client, &wl_buffer_interface,
                                          wl_resource_get_version(r), id);
    wl_resource_set_implementation(res, &buffer_impl, b, nullptr);
    pool->refs++;
}
static void shm_pool_destroy(wl_client*, wl_resource* r) {
    ShmPool* pool = static_cast<ShmPool*>(wl_resource_get_user_data(r));
    if (!pool) return;
    pool->destroyed = true;
    pool->res = nullptr;
    if (pool->refs <= 0) {
        if (pool->data) munmap(pool->data, pool->size);
        if (pool->fd >= 0) close(pool->fd);
        delete pool;
    }
}
static void shm_pool_resize(wl_client*, wl_resource* r, int32_t size) {
    ShmPool* pool = static_cast<ShmPool*>(wl_resource_get_user_data(r));
    if (size <= 0) return;
    if (pool->data) munmap(pool->data, pool->size);
    pool->size = size;
    pool->data = mmap(nullptr, size, PROT_READ, MAP_SHARED, pool->fd, 0);
    if (pool->data == MAP_FAILED) pool->data = nullptr;
}
static const struct wl_shm_pool_interface shm_pool_impl = {
    shm_pool_create_buffer, shm_pool_destroy, shm_pool_resize
};

static void shm_create_pool(wl_client* client, wl_resource*, uint32_t id,
                            int32_t fd, int32_t size) {
    ShmPool* pool = new ShmPool();
    pool->fd = fd;
    pool->size = size;
    pool->data = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (pool->data == MAP_FAILED) {
        pool->data = nullptr;
        close(fd);
        delete pool;
        return;
    }
    pool->res = wl_resource_create(client, &wl_shm_pool_interface, 1, id);
    wl_resource_set_implementation(pool->res, &shm_pool_impl, pool, nullptr);
}
static void shm_release(wl_client*, wl_resource*) {}
static const struct wl_shm_interface shm_impl = { shm_create_pool, shm_release };

// ---- zwp_linux_dmabuf_v1 ---------------------------------------------------

#if HAVE_DMABUF
struct DmabufParams {
    std::vector<DmaBufPlaneStore> planes;
    std::vector<uint64_t> modifiers;
};

static void free_params(DmabufParams* p) {
    if (!p) return;
    for (auto& pl : p->planes) {
        if (pl.fd >= 0) close(pl.fd);
    }
    delete p;
}

static Buffer* make_dmabuf_buffer(DmabufParams* p, int width, int height, uint32_t format) {
    if (!p || width <= 0 || height <= 0 || p->planes.empty() || p->planes.size() > 4) {
        return nullptr;
    }
    Buffer* b = new Buffer();
    b->is_dmabuf = true;
    b->width = width;
    b->height = height;
    b->format = format;
    b->nplanes = static_cast<int>(p->planes.size());
    for (int i = 0; i < b->nplanes; ++i) {
        b->planes[i] = p->planes[i];
        p->planes[i].fd = -1;  // ownership transferred to the buffer
    }
    b->modifier = p->modifiers.empty() ? 0 : p->modifiers[0];
    return b;
}

static void params_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

static void params_add(wl_client*, wl_resource* r, int32_t fd, uint32_t plane_idx,
                       uint32_t offset, uint32_t stride, uint32_t mod_hi, uint32_t mod_lo) {
    DmabufParams* p = static_cast<DmabufParams*>(wl_resource_get_user_data(r));
    if (!p || plane_idx >= 4) { close(fd); return; }
    if (p->planes.size() <= plane_idx) {
        p->planes.resize(plane_idx + 1);
        p->modifiers.resize(plane_idx + 1);
    }
    if (p->planes[plane_idx].fd >= 0) close(p->planes[plane_idx].fd);
    p->planes[plane_idx].fd = fd;
    p->planes[plane_idx].offset = offset;
    p->planes[plane_idx].stride = stride;
    p->modifiers[plane_idx] = (static_cast<uint64_t>(mod_hi) << 32) | mod_lo;
}

static void params_create(wl_client* client, wl_resource* r, int32_t width, int32_t height,
                          uint32_t format, uint32_t) {
    DmabufParams* p = static_cast<DmabufParams*>(wl_resource_get_user_data(r));
    Buffer* b = make_dmabuf_buffer(p, width, height, format);
    if (!b) { zwp_linux_buffer_params_v1_send_failed(r); return; }
    wl_resource* res = wl_resource_create(client, &wl_buffer_interface, 1, 0);
    wl_resource_set_implementation(res, &buffer_impl, b, nullptr);
    zwp_linux_buffer_params_v1_send_created(r, res);
}

static void params_create_immed(wl_client* client, wl_resource* r, uint32_t buffer_id,
                                int32_t width, int32_t height, uint32_t format, uint32_t) {
    DmabufParams* p = static_cast<DmabufParams*>(wl_resource_get_user_data(r));
    Buffer* b = make_dmabuf_buffer(p, width, height, format);
    wl_resource* res = wl_resource_create(client, &wl_buffer_interface, 1, buffer_id);
    wl_resource_set_implementation(res, &buffer_impl, b, nullptr);
    if (!b) {
        zwp_linux_buffer_params_v1_send_failed(r);
    } else {
        zwp_linux_buffer_params_v1_send_created(r, res);
    }
}

static const struct zwp_linux_buffer_params_v1_interface params_impl = {
    params_destroy, params_add, params_create, params_create_immed
};

static void dmabuf_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

static void dmabuf_create_params(wl_client* client, wl_resource* r, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &zwp_linux_buffer_params_v1_interface,
                                          wl_resource_get_version(r), id);
    wl_resource_set_implementation(res, &params_impl, new DmabufParams(),
                                   [](wl_resource* rr) {
                                       free_params(static_cast<DmabufParams*>(
                                           wl_resource_get_user_data(rr)));
                                   });
}

static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
    dmabuf_destroy, dmabuf_create_params, nullptr, nullptr
};
#endif  // HAVE_DMABUF

// ---- xdg_toplevel ----------------------------------------------------------

static void tl_destroy(wl_client*, wl_resource* r) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    if (s) s->toplevel = nullptr;
    wl_resource_destroy(r);
}
static void tl_set_parent(wl_client*, wl_resource*, wl_resource*) {}
static void tl_set_title(wl_client*, wl_resource* r, const char* title) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    if (s) s->title = title ? title : "";
}
static void tl_set_app_id(wl_client*, wl_resource*, const char*) {}
static void tl_show_window_menu(wl_client*, wl_resource*, wl_resource*, uint32_t, int32_t, int32_t) {}
static void tl_move(wl_client*, wl_resource*, wl_resource*, uint32_t) {}
static void tl_resize(wl_client*, wl_resource*, wl_resource*, uint32_t, uint32_t) {}
static void tl_set_max_size(wl_client*, wl_resource* r, int32_t w, int32_t h) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    if (s) { s->max_w = w; s->max_h = h; }
}
static void tl_set_min_size(wl_client*, wl_resource* r, int32_t w, int32_t h) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    if (s) { s->min_w = w; s->min_h = h; }
}
static void tl_set_maximized(wl_client*, wl_resource* r) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    s->maximized = true;
    toplevel_send_configure(s);
}
static void tl_unset_maximized(wl_client*, wl_resource* r) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    s->maximized = false;
    toplevel_send_configure(s);
}
static void tl_set_fullscreen(wl_client*, wl_resource*, wl_resource*) {}
static void tl_unset_fullscreen(wl_client*, wl_resource*) {}
static void tl_set_minimized(wl_client*, wl_resource*) {}
static const struct xdg_toplevel_interface toplevel_impl = {
    tl_destroy, tl_set_parent, tl_set_title, tl_set_app_id, tl_show_window_menu,
    tl_move, tl_resize, tl_set_max_size, tl_set_min_size, tl_set_maximized,
    tl_unset_maximized, tl_set_fullscreen, tl_unset_fullscreen, tl_set_minimized
};

// ---- xdg_popup -------------------------------------------------------------

static void popup_destroy(wl_client*, wl_resource* r) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    if (s) s->popup = nullptr;
    wl_resource_destroy(r);
}
static void popup_grab(wl_client*, wl_resource*, wl_resource*, uint32_t) {}
static void popup_reposition(wl_client*, wl_resource* r, wl_resource*, uint32_t token) {
    xdg_popup_send_repositioned(r, token);
}
static const struct xdg_popup_interface popup_impl = {
    popup_destroy, popup_grab, popup_reposition
};

// ---- xdg_surface -----------------------------------------------------------

static void xdg_surface_destroy(wl_client*, wl_resource* r) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    if (s) s->xdg_surface = nullptr;
    wl_resource_destroy(r);
}
static void toplevel_send_configure(Surface* s) {
    if (!s || !s->toplevel || !s->xdg_surface) return;
    Impl* impl = s->impl;
    uint32_t serial = wl_display_next_serial(impl->display);
    struct wl_array states;
    wl_array_init(&states);
    uint32_t* st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
    if (st) *st = XDG_TOPLEVEL_STATE_ACTIVATED;
    if (s->maximized) {
        uint32_t* st2 = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
        if (st2) *st2 = XDG_TOPLEVEL_STATE_MAXIMIZED;
    }
    xdg_toplevel_send_configure(s->toplevel, impl->width, impl->height, &states);
    wl_array_release(&states);
    xdg_surface_send_configure(s->xdg_surface, serial);
}
static void xdg_surface_get_toplevel(wl_client* client, wl_resource* r, uint32_t id) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    s->toplevel = wl_resource_create(client, &xdg_toplevel_interface,
                                     wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->toplevel, &toplevel_impl, s, nullptr);
    s->is_toplevel = true;
}
static void xdg_surface_get_popup(wl_client* client, wl_resource* r, uint32_t id,
                                  wl_resource* parent, wl_resource* positioner) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    Surface* ps = parent ? static_cast<Surface*>(wl_resource_get_user_data(parent)) : nullptr;
    PositionerData* pd = positioner ? static_cast<PositionerData*>(
        wl_resource_get_user_data(positioner)) : nullptr;

    uint32_t ver = std::min<uint32_t>(wl_resource_get_version(r), 6);
    wl_resource* res = wl_resource_create(client, &xdg_popup_interface, ver, id);
    wl_resource_set_implementation(res, &popup_impl, s, nullptr);
    s->is_popup = true;
    s->popup = res;
    if (ps) {
        // Composite the popup over its parent surface (offsets are relative to
        // the parent), like a subsurface.
        s->sub_parent = ps->res;
        s->sub_x = pd ? pd->ax + pd->ox : 0;
        s->sub_y = pd ? pd->ay + pd->oy : 0;
    }
    // Configure/map the popup so the client doesn't wait forever. A zero size
    // lets the client pick the popup's own size.
    int pw = (pd && pd->w > 0) ? pd->w : 0;
    int ph = (pd && pd->h > 0) ? pd->h : 0;
    xdg_popup_send_configure(res, s->sub_x, s->sub_y, pw, ph);
    uint32_t serial = wl_display_next_serial(s->impl->display);
    xdg_surface_send_configure(r, serial);
}
static void xdg_surface_set_window_geometry(wl_client*, wl_resource* r,
                                            int32_t, int32_t, int32_t w, int32_t h) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    s->win_w = w;
    s->win_h = h;
}
static void xdg_surface_ack_configure(wl_client*, wl_resource* r, uint32_t) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    s->configured = true;
}
static const struct xdg_surface_interface xdg_surface_impl = {
    xdg_surface_destroy, xdg_surface_get_toplevel, xdg_surface_get_popup,
    xdg_surface_set_window_geometry, xdg_surface_ack_configure
};

// ---- xdg_wm_base -----------------------------------------------------------

static void wm_base_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void positioner_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void positioner_set_size(wl_client*, wl_resource* r, int32_t w, int32_t h) {
    PositionerData* p = static_cast<PositionerData*>(wl_resource_get_user_data(r));
    if (p) { p->w = w; p->h = h; }
}
static void positioner_set_anchor_rect(wl_client*, wl_resource* r, int32_t x, int32_t y,
                                       int32_t w, int32_t h) {
    PositionerData* p = static_cast<PositionerData*>(wl_resource_get_user_data(r));
    if (p) { p->ax = x; p->ay = y; p->aw = w; p->ah = h; }
}
static void positioner_set_anchor(wl_client*, wl_resource*, uint32_t) {}
static void positioner_set_gravity(wl_client*, wl_resource*, uint32_t) {}
static void positioner_set_constraint_adjustment(wl_client*, wl_resource*, uint32_t) {}
static void positioner_set_offset(wl_client*, wl_resource* r, int32_t x, int32_t y) {
    PositionerData* p = static_cast<PositionerData*>(wl_resource_get_user_data(r));
    if (p) { p->ox = x; p->oy = y; }
}
static void positioner_set_reactive(wl_client*, wl_resource*) {}
static void positioner_set_parent_size(wl_client*, wl_resource*, int32_t, int32_t) {}
static void positioner_set_parent_configure(wl_client*, wl_resource*, uint32_t) {}
static const struct xdg_positioner_interface positioner_impl = {
    positioner_destroy, positioner_set_size, positioner_set_anchor_rect,
    positioner_set_anchor, positioner_set_gravity,
    positioner_set_constraint_adjustment, positioner_set_offset,
    positioner_set_reactive, positioner_set_parent_size, positioner_set_parent_configure
};
static void wm_base_create_positioner(wl_client* client, wl_resource* r, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &xdg_positioner_interface,
                                          wl_resource_get_version(r), id);
    wl_resource_set_implementation(res, &positioner_impl, new PositionerData(),
                                   [](wl_resource* rr) {
                                       delete static_cast<PositionerData*>(
                                           wl_resource_get_user_data(rr));
                                   });
}
static void wm_base_get_xdg_surface(wl_client* client, wl_resource* wm, uint32_t id,
                                    wl_resource* surface_res) {
    Impl* impl = static_cast<Impl*>(wl_resource_get_user_data(wm));
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(surface_res));
    s->impl = impl;
    s->res = surface_res;
    wl_resource* res = wl_resource_create(client, &xdg_surface_interface,
                                          wl_resource_get_version(wm), id);
    wl_resource_set_implementation(res, &xdg_surface_impl, s, nullptr);
    s->xdg_surface = res;
}
static void wm_base_pong(wl_client*, wl_resource*, uint32_t) {}
static const struct xdg_wm_base_interface wm_base_impl = {
    wm_base_destroy, wm_base_create_positioner, wm_base_get_xdg_surface, wm_base_pong
};

// ---- wl_output -------------------------------------------------------------

static void output_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static const struct wl_output_interface output_impl = { output_release };

// ---- wl_data_device_manager (minimal) --------------------------------------

static void data_source_offer(wl_client*, wl_resource*, const char*) {}
static void data_source_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void data_source_set_actions(wl_client*, wl_resource*, uint32_t) {}
static const struct wl_data_source_interface data_source_impl = {
    data_source_offer, data_source_destroy, data_source_set_actions
};

static void data_device_start_drag(wl_client*, wl_resource*, wl_resource*, wl_resource*, wl_resource*, uint32_t) {}
static void data_device_set_selection(wl_client*, wl_resource*, wl_resource*, uint32_t) {}
static void data_device_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static const struct wl_data_device_interface data_device_impl = {
    data_device_start_drag, data_device_set_selection, data_device_release
};

static void ddm_create_data_source(wl_client* client, wl_resource*, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &wl_data_source_interface, 3, id);
    wl_resource_set_implementation(res, &data_source_impl, nullptr, nullptr);
}
static void ddm_get_data_device(wl_client* client, wl_resource*, uint32_t id, wl_resource*) {
    wl_resource* res = wl_resource_create(client, &wl_data_device_interface, 3, id);
    wl_resource_set_implementation(res, &data_device_impl, nullptr, nullptr);
}
static const struct wl_data_device_manager_interface ddm_impl = {
    ddm_create_data_source, ddm_get_data_device
};

// ---- wl_subcompositor (minimal) --------------------------------------------

static void subsurface_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void subsurface_set_position(wl_client*, wl_resource* r, int32_t x, int32_t y) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    if (s) { s->sub_x = x; s->sub_y = y; }
}
static void subsurface_place(wl_client*, wl_resource*, wl_resource*) {}
static void subsurface_set_sync(wl_client*, wl_resource*) {}
static void subsurface_set_desync(wl_client*, wl_resource*) {}
static const struct wl_subsurface_interface subsurface_impl = {
    subsurface_destroy, subsurface_set_position, subsurface_place, subsurface_place,
    subsurface_set_sync, subsurface_set_desync
};
static void subcompositor_get_subsurface(wl_client* client, wl_resource*, uint32_t id,
                                         wl_resource* surface_res, wl_resource* parent_res) {
    Surface* child = static_cast<Surface*>(wl_resource_get_user_data(surface_res));
    if (child) {
        child->sub_parent = parent_res;
        child->is_subsurface = true;
    }
    wl_resource* res = wl_resource_create(client, &wl_subsurface_interface, 1, id);
    wl_resource_set_implementation(res, &subsurface_impl, child, nullptr);
}
static void subcompositor_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static const struct wl_subcompositor_interface subcompositor_impl = {
    subcompositor_destroy, subcompositor_get_subsurface
};

// ---- wl_seat ---------------------------------------------------------------

static void seat_res_destroy(wl_resource* r) { delete static_cast<SeatRes*>(wl_resource_get_user_data(r)); }
static void keyboard_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static const struct wl_keyboard_interface keyboard_impl = { keyboard_release };
static void pointer_set_cursor(wl_client*, wl_resource*, uint32_t, wl_resource*, int32_t, int32_t) {}
static void pointer_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static const struct wl_pointer_interface pointer_impl = { pointer_set_cursor, pointer_release };

static void send_keymap(Impl* impl, wl_resource* kb) {
    if (!impl->xkb_map) return;
    char* str = xkb_keymap_get_as_string(impl->xkb_map, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!str) return;
    size_t len = std::strlen(str) + 1;
    int fd = memfd_create("renderterm-keymap", MFD_CLOEXEC);
    if (fd >= 0) {
        if (ftruncate(fd, len) == 0) {
            void* m = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (m != MAP_FAILED) {
                std::memcpy(m, str, len);
                munmap(m, len);
                wl_keyboard_send_keymap(kb, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, len);
                // The fd is referenced by the queued message until the next
                // flush, so send it before closing.
                wl_display_flush_clients(impl->display);
            }
        }
        close(fd);
    }
    free(str);
}

// A client may open several Wayland connections, so focal input must be
// resolved within the connection that owns the input resource.
static Surface* find_toplevel_for(Impl* impl, wl_client* client) {
    for (Surface* s : impl->surfaces) {
        if (s->is_toplevel && s->xdg_surface && wl_resource_get_client(s->res) == client)
            return s;
    }
    return nullptr;
}

static void send_keyboard_focus(Impl* impl, InputRes* ir, wl_resource* res, wl_resource* surface) {
    struct wl_array keys;
    wl_array_init(&keys);
    uint32_t serial = wl_display_next_serial(impl->display);
    wl_keyboard_send_enter(res, serial, surface, &keys);
    wl_array_release(&keys);
    uint32_t mserial = wl_display_next_serial(impl->display);
    wl_keyboard_send_modifiers(res, mserial, 0, 0, 0, 0);
    ir->entered = true;
}

static void send_pointer_focus(Impl* impl, InputRes* ir, wl_resource* res, wl_resource* surface) {
    uint32_t serial = wl_display_next_serial(impl->display);
    wl_pointer_send_enter(res, serial, surface, wl_fixed_from_int(0), wl_fixed_from_int(0));
    pointer_frame(res);
    ir->entered = true;
}

static void input_res_destroy(wl_resource* r) {
    InputRes* ir = static_cast<InputRes*>(wl_resource_get_user_data(r));
    if (!ir) return;
    Impl* impl = ir->impl;
    if (impl) {
        for (auto it = impl->keyboards.begin(); it != impl->keyboards.end(); ++it) {
            if (*it == r) { impl->keyboards.erase(it); break; }
        }
        for (auto it = impl->pointers.begin(); it != impl->pointers.end(); ++it) {
            if (*it == r) { impl->pointers.erase(it); break; }
        }
    }
    delete ir;
}

static void seat_get_pointer(wl_client* client, wl_resource* r, uint32_t id) {
    SeatRes* sr = static_cast<SeatRes*>(wl_resource_get_user_data(r));
    InputRes* ir = new InputRes();
    ir->impl = sr->impl;
    wl_resource* res = wl_resource_create(client, &wl_pointer_interface,
                                          std::min<uint32_t>(sr->version, 7), id);
    wl_resource_set_implementation(res, &pointer_impl, ir, input_res_destroy);
    sr->impl->pointers.push_back(res);
    Surface* top = find_toplevel_for(sr->impl, client);
    if (top) send_pointer_focus(sr->impl, ir, res, top->res);
}
static void seat_get_keyboard(wl_client* client, wl_resource* r, uint32_t id) {
    SeatRes* sr = static_cast<SeatRes*>(wl_resource_get_user_data(r));
    InputRes* ir = new InputRes();
    ir->impl = sr->impl;
    wl_resource* res = wl_resource_create(client, &wl_keyboard_interface,
                                          std::min<uint32_t>(sr->version, 7), id);
    wl_resource_set_implementation(res, &keyboard_impl, ir, input_res_destroy);
    sr->impl->keyboards.push_back(res);
    send_keymap(sr->impl, res);
    wl_keyboard_send_repeat_info(res, 25, 600);
    // Give it focus if a toplevel already exists on this connection.
    Surface* top = find_toplevel_for(sr->impl, client);
    if (top) send_keyboard_focus(sr->impl, ir, res, top->res);
}
static void seat_get_touch(wl_client*, wl_resource*, uint32_t) {}
static void seat_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static const struct wl_seat_interface seat_impl = {
    seat_get_pointer, seat_get_keyboard, seat_get_touch, seat_release
};

// ---- global bind functions -------------------------------------------------

static void bind_compositor(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &wl_compositor_interface,
                                          std::min<uint32_t>(version, 4), id);
    wl_resource_set_implementation(res, &compositor_impl, data, nullptr);
}
static void bind_ddm(wl_client* client, void*, uint32_t version, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &wl_data_device_manager_interface,
                                          std::min<uint32_t>(version, 3), id);
    wl_resource_set_implementation(res, &ddm_impl, nullptr, nullptr);
}
static void bind_subcompositor(wl_client* client, void*, uint32_t version, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &wl_subcompositor_interface,
                                          std::min<uint32_t>(version, 1), id);
    wl_resource_set_implementation(res, &subcompositor_impl, nullptr, nullptr);
}
static void bind_shm(wl_client* client, void*, uint32_t version, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &wl_shm_interface,
                                          std::min<uint32_t>(version, 1), id);
    wl_resource_set_implementation(res, &shm_impl, nullptr, nullptr);
    wl_shm_send_format(res, WL_SHM_FORMAT_ARGB8888);
    wl_shm_send_format(res, WL_SHM_FORMAT_XRGB8888);
}
static void bind_output(wl_client* client, void* data, uint32_t version, uint32_t id) {
    Impl* impl = static_cast<Impl*>(data);
    wl_resource* res = wl_resource_create(client, &wl_output_interface,
                                          std::min<uint32_t>(version, 3), id);
    wl_resource_set_implementation(res, &output_impl, nullptr, nullptr);
    wl_output_send_geometry(res, 0, 0, 300, 200, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                            "renderterm", "renderterm", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(res, WL_OUTPUT_MODE_CURRENT, impl->width, impl->height, 60000);
    wl_output_send_scale(res, 1);
    wl_output_send_done(res);
}
static void bind_seat(wl_client* client, void* data, uint32_t version, uint32_t id) {
    Impl* impl = static_cast<Impl*>(data);
    SeatRes* sr = new SeatRes();
    sr->impl = impl;
    sr->version = std::min<uint32_t>(version, 7);
    wl_resource* res = wl_resource_create(client, &wl_seat_interface, sr->version, id);
    wl_resource_set_implementation(res, &seat_impl, sr, seat_res_destroy);
    wl_seat_send_capabilities(res, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
    if (version >= WL_SEAT_NAME_SINCE_VERSION) wl_seat_send_name(res, "seat0");
}
static void bind_wm_base(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &xdg_wm_base_interface,
                                          std::min<uint32_t>(version, 6), id);
    wl_resource_set_implementation(res, &wm_base_impl, data, nullptr);
}

#if HAVE_DMABUF
static void bind_dmabuf(wl_client* client, void* data, uint32_t version, uint32_t id) {
    Impl* impl = static_cast<Impl*>(data);
    wl_resource* res = wl_resource_create(client, &zwp_linux_dmabuf_v1_interface,
                                          std::min<uint32_t>(version, 3), id);
    wl_resource_set_implementation(res, &dmabuf_impl, impl, nullptr);
    if (impl->importer) {
        for (uint32_t fcc : impl->importer->formats()) {
            zwp_linux_dmabuf_v1_send_format(res, fcc);
            for (uint64_t mod : impl->importer->modifiers(fcc)) {
                zwp_linux_dmabuf_v1_send_modifier(res, fcc,
                                                  static_cast<uint32_t>(mod >> 32),
                                                  static_cast<uint32_t>(mod & 0xffffffffu));
            }
        }
    }
}
#endif

}  // namespace

// ---------------------------------------------------------------------------

WaylandServer::WaylandServer(int width, int height) : m_width(width), m_height(height) {}
WaylandServer::~WaylandServer() { stop(); }

bool WaylandServer::start() {
    Impl* impl = new Impl();
    impl->owner = this;
    impl->width = m_width;
    impl->height = m_height;
    m_impl = impl;

    char tmpl[] = "/tmp/renderterm-wl-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) { delete impl; m_impl = nullptr; return false; }
    m_runtime_dir = dir;
    chmod(m_runtime_dir.c_str(), 0700);
    setenv("XDG_RUNTIME_DIR", m_runtime_dir.c_str(), 1);

    impl->display = wl_display_create();
    if (!impl->display) return false;
    impl->loop = wl_display_get_event_loop(impl->display);

    impl->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (impl->xkb_ctx) {
        impl->xkb_map = xkb_keymap_new_from_names(impl->xkb_ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (impl->xkb_map) impl->xkb_st = xkb_state_new(impl->xkb_map);
    }

    wl_global_create(impl->display, &wl_compositor_interface, 4, impl, bind_compositor);
    wl_global_create(impl->display, &wl_shm_interface, 1, impl, bind_shm);
    wl_global_create(impl->display, &wl_output_interface, 3, impl, bind_output);
    wl_global_create(impl->display, &wl_seat_interface, 8, impl, bind_seat);
    wl_global_create(impl->display, &xdg_wm_base_interface, 6, impl, bind_wm_base);
    wl_global_create(impl->display, &wl_data_device_manager_interface, 3, impl, bind_ddm);
    wl_global_create(impl->display, &wl_subcompositor_interface, 1, impl, bind_subcompositor);
#if HAVE_DMABUF
    impl->importer = new EglImporter();
    if (impl->importer->init() && impl->importer->available()) {
        wl_global_create(impl->display, &zwp_linux_dmabuf_v1_interface, 3, impl, bind_dmabuf);
    } else {
        delete impl->importer;
        impl->importer = nullptr;
    }
#endif

    const char* sock = wl_display_add_socket_auto(impl->display);
    if (sock) {
        m_socket_name = sock;
    } else if (wl_display_add_socket(impl->display, "wayland-rt") == 0) {
        m_socket_name = "wayland-rt";
    } else {
        return false;
    }

    setenv("WAYLAND_DISPLAY", m_socket_name.c_str(), 1);
    wl_display_flush_clients(impl->display);
    return true;
}

void WaylandServer::stop() {
    Impl* impl = static_cast<Impl*>(m_impl);
    if (!impl) return;
    if (impl->display) {
        wl_display_destroy_clients(impl->display);
        wl_display_destroy(impl->display);
    }
    if (impl->xkb_st) xkb_state_unref(impl->xkb_st);
    if (impl->xkb_map) xkb_keymap_unref(impl->xkb_map);
    if (impl->xkb_ctx) xkb_context_unref(impl->xkb_ctx);
#if HAVE_DMABUF
    delete impl->importer;
    impl->importer = nullptr;
#endif
    // Surfaces are owned by their wl_surface resources and freed by the
    // resource destructor during wl_display_destroy_clients().
    if (!m_runtime_dir.empty()) rmdir(m_runtime_dir.c_str());
    delete impl;
    m_impl = nullptr;
}

bool WaylandServer::poll(int timeout_ms) {
    Impl* impl = static_cast<Impl*>(m_impl);
    if (!impl || !impl->display) return false;
    if (wl_event_loop_dispatch(impl->loop, timeout_ms) < 0) return false;

    // Deliver pending frame callbacks, throttled to ~60Hz, so clients that wait
    // for a frame callback before drawing will still paint.
    uint32_t now = now_ms();
    if (now - impl->last_frame_ms >= 16) {
        impl->last_frame_ms = now;
        for (Surface* s : impl->surfaces) {
            if (s->frame_cb) {
                wl_callback_send_done(s->frame_cb, now);
                wl_resource_destroy(s->frame_cb);
                s->frame_cb = nullptr;
            }
        }
    }

    wl_display_flush_clients(impl->display);
    return true;
}

bool WaylandServer::capture(Frame& out) {
    Impl* impl = static_cast<Impl*>(m_impl);
    if (!impl || !impl->has_frame) return false;
    out = impl->frame;
    return true;
}

bool WaylandServer::toplevel_resizable(unsigned long id) const {
    Impl* impl = static_cast<Impl*>(m_impl);
    if (!impl) return false;
    for (Surface* s : impl->surfaces) {
        if (reinterpret_cast<unsigned long>(s->res) == id &&
            s->is_toplevel && s->xdg_surface) {
            bool fixed_w = s->min_w > 0 && s->max_w > 0 && s->min_w == s->max_w;
            bool fixed_h = s->min_h > 0 && s->max_h > 0 && s->min_h == s->max_h;
            return !(fixed_w && fixed_h);
        }
    }
    return false;
}

bool WaylandServer::toplevel_can_close(unsigned long id) const {
    Impl* impl = static_cast<Impl*>(m_impl);
    if (!impl) return false;
    for (Surface* s : impl->surfaces) {
        if (reinterpret_cast<unsigned long>(s->res) == id &&
            s->is_toplevel && s->xdg_surface && s->toplevel) {
            return true;
        }
    }
    return false;
}

void WaylandServer::close_toplevel(unsigned long id) {
    Impl* impl = static_cast<Impl*>(m_impl);
    if (!impl) return;
    for (Surface* s : impl->surfaces) {
        if (reinterpret_cast<unsigned long>(s->res) == id &&
            s->is_toplevel && s->xdg_surface && s->toplevel) {
            xdg_toplevel_send_close(s->toplevel);
            wl_display_flush_clients(impl->display);
            return;
        }
    }
}

bool WaylandServer::resize_toplevel(unsigned long id, int w, int h) {
    Impl* impl = static_cast<Impl*>(m_impl);
    if (!impl || w <= 0 || h <= 0) return false;
    for (Surface* s : impl->surfaces) {
        if (reinterpret_cast<unsigned long>(s->res) == id &&
            s->is_toplevel && s->xdg_surface && s->toplevel) {
            uint32_t serial = wl_display_next_serial(impl->display);
            struct wl_array states;
            wl_array_init(&states);
            uint32_t* st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
            if (st) *st = XDG_TOPLEVEL_STATE_ACTIVATED;
            xdg_toplevel_send_configure(s->toplevel, w, h, &states);
            wl_array_release(&states);
            xdg_surface_send_configure(s->xdg_surface, serial);
            wl_display_flush_clients(impl->display);
            return true;
        }
    }
    return false;
}

std::string WaylandServer::toplevel_title(unsigned long id) const {
    Impl* impl = static_cast<Impl*>(m_impl);
    if (!impl) return {};
    for (Surface* s : impl->surfaces) {
        if (reinterpret_cast<unsigned long>(s->res) == id &&
            s->is_toplevel && s->xdg_surface) {
            return s->title;
        }
    }
    return {};
}

void WaylandServer::toplevels(std::vector<unsigned long>& out) const {
    out.clear();
    Impl* impl = static_cast<Impl*>(m_impl);
    if (!impl) return;
    for (Surface* s : impl->surfaces) {
        if (s->is_toplevel && s->xdg_surface) {
            out.push_back(reinterpret_cast<unsigned long>(s->res));
        }
    }
    std::sort(out.begin(), out.end());
}

bool WaylandServer::capture_toplevel(unsigned long id, Frame& out) {
    Impl* impl = static_cast<Impl*>(m_impl);
    if (!impl) return false;
    for (Surface* s : impl->surfaces) {
        if (reinterpret_cast<unsigned long>(s->res) == id &&
            s->is_toplevel && s->xdg_surface) {
            return recompose(impl, s, out);
        }
    }
    return false;
}

bool WaylandServer::has_toplevel() const {
    Impl* impl = static_cast<Impl*>(m_impl);
    if (!impl) return false;
    for (Surface* s : impl->surfaces) if (s->is_toplevel && s->xdg_surface) return true;
    return false;
}

bool WaylandServer::input_available() const { return m_impl != nullptr; }

void WaylandServer::key(unsigned int keysym, bool press, bool ctrl, bool alt, bool shift) {
    Impl* impl = static_cast<Impl*>(m_impl);
    if (!impl || !impl->xkb_map || impl->keyboards.empty()) return;

    auto find = [&](xkb_keysym_t ks, bool& need_shift) -> xkb_keycode_t {
        for (xkb_keycode_t kc = 8; kc < 256; ++kc) {
            const xkb_keysym_t* syms = nullptr;
            int n = xkb_keymap_key_get_syms_by_level(impl->xkb_map, kc, 0, 0, &syms);
            for (int i = 0; i < n; ++i) if (syms[i] == ks) { need_shift = false; return kc; }
            n = xkb_keymap_key_get_syms_by_level(impl->xkb_map, kc, 0, 1, &syms);
            for (int i = 0; i < n; ++i) if (syms[i] == ks) { need_shift = true; return kc; }
        }
        return 0;
    };
    // `evdev` is the Linux keycode carried by wl_keyboard.key; xkbcommon uses
    // keycodes shifted by 8 (X11 convention), so convert when touching state.
    auto send_key = [&](xkb_keycode_t evdev, bool down) {
        if (!evdev) return;
        uint32_t serial = wl_display_next_serial(impl->display);
        uint32_t t = now_ms();
        for (wl_resource* kb : impl->keyboards)
            wl_keyboard_send_key(kb, serial, t, evdev,
                                 down ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
        if (impl->xkb_st) {
            xkb_state_update_key(impl->xkb_st, evdev + 8, down ? XKB_KEY_DOWN : XKB_KEY_UP);
            uint32_t dep = xkb_state_serialize_mods(impl->xkb_st, XKB_STATE_MODS_DEPRESSED);
            uint32_t lat = xkb_state_serialize_mods(impl->xkb_st, XKB_STATE_MODS_LATCHED);
            uint32_t loc = xkb_state_serialize_mods(impl->xkb_st, XKB_STATE_MODS_LOCKED);
            uint32_t grp = xkb_state_serialize_layout(impl->xkb_st, XKB_STATE_LAYOUT_EFFECTIVE);
            for (wl_resource* kb : impl->keyboards)
                wl_keyboard_send_modifiers(kb, serial, dep, lat, loc, grp);
        }
    };

    bool need_shift = false;
    xkb_keycode_t xkc = find(keysym, need_shift);
    if (!xkc) return;
    xkb_keycode_t kc = xkc - 8;  // xkb keycode -> evdev keycode
    if (!press) { send_key(kc, false); return; }
    if (ctrl) send_key(29, true);
    if (alt) send_key(56, true);
    if (shift || need_shift) send_key(42, true);
    send_key(kc, true);
    send_key(kc, false);
    if (shift || need_shift) send_key(42, false);
    if (alt) send_key(56, false);
    if (ctrl) send_key(29, false);
    wl_display_flush_clients(impl->display);
}

void WaylandServer::pointer_motion(int x, int y) {
    Impl* impl = static_cast<Impl*>(m_impl);
    if (!impl || impl->pointers.empty()) return;
    uint32_t t = now_ms();
    for (wl_resource* p : impl->pointers) {
        InputRes* ir = static_cast<InputRes*>(wl_resource_get_user_data(p));
        Surface* target = find_toplevel_for(impl, wl_resource_get_client(p));
        if (!target) continue;
        if (ir && !ir->entered) send_pointer_focus(impl, ir, p, target->res);
        wl_pointer_send_motion(p, t, wl_fixed_from_int(x), wl_fixed_from_int(y));
        pointer_frame(p);
    }
    wl_display_flush_clients(impl->display);
}

void WaylandServer::button(int xbutton, bool press) {
    Impl* impl = static_cast<Impl*>(m_impl);
    if (!impl || impl->pointers.empty()) return;
    uint32_t code = 0x110;
    if (xbutton == 2) code = 0x112; else if (xbutton == 3) code = 0x111;
    uint32_t serial = wl_display_next_serial(impl->display);
    uint32_t t = now_ms();
    for (wl_resource* p : impl->pointers) {
        wl_pointer_send_button(p, serial, t, code,
                               press ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
        pointer_frame(p);
    }
    wl_display_flush_clients(impl->display);
}

void WaylandServer::wheel(int dir) {
    Impl* impl = static_cast<Impl*>(m_impl);
    if (!impl || impl->pointers.empty()) return;
    uint32_t t = now_ms();
    for (wl_resource* p : impl->pointers) {
        wl_pointer_send_axis(p, t, WL_POINTER_AXIS_VERTICAL_SCROLL,
                             wl_fixed_from_double(dir > 0 ? -10.0 : 10.0));
        pointer_frame(p);
    }
    wl_display_flush_clients(impl->display);
}

#else  // !HAVE_WAYLAND

WaylandServer::WaylandServer(int, int) {}
WaylandServer::~WaylandServer() {}
bool WaylandServer::start() { return false; }
void WaylandServer::stop() {}
bool WaylandServer::poll(int) { return false; }
bool WaylandServer::capture(Frame&) { return false; }
void WaylandServer::toplevels(std::vector<unsigned long>& out) const { out.clear(); }
std::string WaylandServer::toplevel_title(unsigned long) const { return {}; }
bool WaylandServer::resize_toplevel(unsigned long, int, int) { return false; }
bool WaylandServer::toplevel_resizable(unsigned long) const { return false; }
bool WaylandServer::toplevel_can_close(unsigned long) const { return false; }
void WaylandServer::close_toplevel(unsigned long) {}
bool WaylandServer::capture_toplevel(unsigned long, Frame&) { return false; }
bool WaylandServer::has_toplevel() const { return false; }
bool WaylandServer::input_available() const { return false; }
void WaylandServer::key(unsigned int, bool, bool, bool, bool) {}
void WaylandServer::pointer_motion(int, int) {}
void WaylandServer::button(int, bool) {}
void WaylandServer::wheel(int) {}

#endif
