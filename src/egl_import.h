#pragma once

#include <cstdint>
#include <vector>
#include "capture.h"

struct DmaBufPlane {
    int fd;
    uint32_t offset;
    uint32_t stride;
};

// Imports linux-dmabuf buffers via EGL and reads them back into an RGB Frame.
class EglImporter {
public:
    EglImporter();
    ~EglImporter();

    bool init();
    bool available() const { return m_ok; }

    bool import(const DmaBufPlane* planes, int nplanes, int width, int height,
                uint32_t fourcc, uint64_t modifier, Frame& out);

    const std::vector<uint32_t>& formats() const { return m_formats; }
    std::vector<uint64_t> modifiers(uint32_t fourcc) const;

private:
    bool m_ok = false;
    void* m_dpy = nullptr;    // EGLDisplay
    void* m_ctx = nullptr;    // EGLContext
    void* m_surf = nullptr;   // EGLSurface (pbuffer)
    void* m_createImage = nullptr;
    void* m_destroyImage = nullptr;
    void* m_imageTargetTex = nullptr;
    std::vector<uint32_t> m_formats;
};
