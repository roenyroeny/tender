#include "egl_import.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace {

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
}
constexpr uint32_t kArgb8888 = fourcc('A', 'R', '2', '4');
constexpr uint32_t kXrgb8888 = fourcc('X', 'R', '2', '4');
constexpr uint32_t kAbgr8888 = fourcc('A', 'B', '2', '4');
constexpr uint32_t kXbgr8888 = fourcc('X', 'B', '2', '4');
constexpr uint64_t kModLinear = 0;
constexpr uint64_t kModInvalid = 0x00ffffffffffffffULL;

void* proc(const char* name) {
    return reinterpret_cast<void*>(eglGetProcAddress(name));
}

}  // namespace

EglImporter::EglImporter() {}
EglImporter::~EglImporter() {
    if (m_dpy) {
        eglMakeCurrent(static_cast<EGLDisplay>(m_dpy), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_surf) eglDestroySurface(static_cast<EGLDisplay>(m_dpy), static_cast<EGLSurface>(m_surf));
        if (m_ctx) eglDestroyContext(static_cast<EGLDisplay>(m_dpy), static_cast<EGLContext>(m_ctx));
        eglTerminate(static_cast<EGLDisplay>(m_dpy));
    }
}

bool EglImporter::init() {
    auto getPlatformDisplay =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(proc("eglGetPlatformDisplayEXT"));
    auto queryDevices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(proc("eglQueryDevicesEXT"));
    auto queryDeviceString =
        reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(proc("eglQueryDeviceStringEXT"));

    EGLDisplay dpy = EGL_NO_DISPLAY;
    if (getPlatformDisplay && queryDevices) {
        EGLint ndev = 0;
        if (queryDevices(0, nullptr, &ndev) && ndev > 0) {
            std::vector<EGLDeviceEXT> devs(ndev);
            if (queryDevices(ndev, devs.data(), &ndev)) {
                EGLDeviceEXT chosen = devs[0];
                for (EGLint i = 0; i < ndev; ++i) {
                    const char* path = queryDeviceString
                        ? queryDeviceString(devs[i], EGL_DRM_DEVICE_FILE_EXT) : nullptr;
                    if (path && std::strstr(path, "renderD")) { chosen = devs[i]; break; }
                }
                dpy = getPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, chosen, nullptr);
            }
        }
    }
    if (dpy == EGL_NO_DISPLAY) {
#ifdef EGL_PLATFORM_SURFACELESS_MESA
        if (getPlatformDisplay) {
            dpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        }
#endif
    }
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) return false;

    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) return false;
    m_dpy = dpy;
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint ncfg = 0;
    if (!eglChooseConfig(dpy, cfg_attribs, &cfg, 1, &ncfg) || ncfg < 1) return false;

    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    m_ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (m_ctx == EGL_NO_CONTEXT) return false;

    EGLint pb_attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    m_surf = eglCreatePbufferSurface(dpy, cfg, pb_attribs);
    if (!eglMakeCurrent(dpy, static_cast<EGLSurface>(m_surf),
                        static_cast<EGLSurface>(m_surf),
                        static_cast<EGLContext>(m_ctx))) {
        return false;
    }

    m_createImage = proc("eglCreateImageKHR");
    m_destroyImage = proc("eglDestroyImageKHR");
    m_imageTargetTex = proc("glEGLImageTargetTexture2DOES");
    if (!m_createImage || !m_destroyImage || !m_imageTargetTex) return false;

    // Supported formats.
    auto queryFormats = reinterpret_cast<PFNEGLQUERYDMABUFFORMATSEXTPROC>(proc("eglQueryDmaBufFormatsEXT"));
    EGLint nfmt = 0;
    if (queryFormats && queryFormats(dpy, 0, nullptr, &nfmt) && nfmt > 0) {
        std::vector<EGLint> fmts(nfmt);
        if (queryFormats(dpy, nfmt, fmts.data(), &nfmt)) {
            for (int i = 0; i < nfmt; ++i) {
                uint32_t f = static_cast<uint32_t>(fmts[i]);
                if (f == kArgb8888 || f == kXrgb8888 || f == kAbgr8888 || f == kXbgr8888) {
                    m_formats.push_back(f);
                }
            }
        }
    }
    if (m_formats.empty()) {
        m_formats = { kArgb8888, kXrgb8888, kAbgr8888, kXbgr8888 };
    }

    m_ok = true;
    return true;
}

std::vector<uint64_t> EglImporter::modifiers(uint32_t fcc) const {
    std::vector<uint64_t> out;
    auto queryMods = reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(proc("eglQueryDmaBufModifiersEXT"));
    if (queryMods && m_dpy) {
        EGLint n = 0;
        if (queryMods(static_cast<EGLDisplay>(m_dpy), static_cast<EGLint>(fcc), 0, nullptr, nullptr, &n) && n > 0) {
            std::vector<uint64_t> mods(n);
            std::vector<EGLBoolean> ext(n);
            if (queryMods(static_cast<EGLDisplay>(m_dpy), static_cast<EGLint>(fcc), n,
                          mods.data(), ext.data(), &n)) {
                for (int i = 0; i < n; ++i) {
                    if (mods[i] != kModInvalid) out.push_back(mods[i]);
                }
            }
        }
    }
    if (out.empty()) out.push_back(kModLinear);
    return out;
}

bool EglImporter::import(const DmaBufPlane* planes, int nplanes, int width, int height,
                         uint32_t fcc, uint64_t modifier, Frame& out) {
    if (!m_ok || nplanes < 1 || nplanes > 4 || width <= 0 || height <= 0) return false;

    EGLint attribs[64];
    int n = 0;
    attribs[n++] = EGL_WIDTH;           attribs[n++] = width;
    attribs[n++] = EGL_HEIGHT;          attribs[n++] = height;
    attribs[n++] = EGL_LINUX_DRM_FOURCC_EXT; attribs[n++] = static_cast<EGLint>(fcc);
    static const EGLint plane_fd[4]   = { EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
                                          EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT };
    static const EGLint plane_off[4]  = { EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
                                          EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT };
    static const EGLint plane_str[4]  = { EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
                                          EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT };
    static const EGLint plane_mlo[4]  = { EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
                                          EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT };
    static const EGLint plane_mhi[4]  = { EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
                                          EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT };
    for (int i = 0; i < nplanes; ++i) {
        attribs[n++] = plane_fd[i];  attribs[n++] = planes[i].fd;
        attribs[n++] = plane_off[i]; attribs[n++] = static_cast<EGLint>(planes[i].offset);
        attribs[n++] = plane_str[i]; attribs[n++] = static_cast<EGLint>(planes[i].stride);
        attribs[n++] = plane_mlo[i]; attribs[n++] = static_cast<EGLint>(modifier & 0xffffffffu);
        attribs[n++] = plane_mhi[i]; attribs[n++] = static_cast<EGLint>(modifier >> 32);
    }
    attribs[n++] = EGL_NONE;

    auto createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(m_createImage);
    auto destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(m_destroyImage);
    auto imageTargetTex = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(m_imageTargetTex);

    EGLDisplay dpy = static_cast<EGLDisplay>(m_dpy);
    EGLImageKHR img = createImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                  nullptr, attribs);
    if (img == EGL_NO_IMAGE_KHR) return false;

    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    imageTargetTex(GL_TEXTURE_2D, static_cast<GLeglImageOES>(img));

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        destroyImage(dpy, img);
        return false;
    }

    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    out.width = width;
    out.height = height;
    out.rgb.resize(static_cast<size_t>(width) * height * 3);
    // The dmabuf's first row is the top of the image (DRM/Wayland convention),
    // and attaching the imported EGLImage as a texture keeps that row order in
    // the framebuffer, so glReadPixels already returns rows top-down.
    for (int y = 0; y < height; ++y) {
        const uint8_t* src = rgba.data() + static_cast<size_t>(y) * width * 4;
        uint8_t* dst = out.rgb.data() + static_cast<size_t>(y) * width * 3;
        for (int x = 0; x < width; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 0];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 2];
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    destroyImage(dpy, img);
    return true;
}
