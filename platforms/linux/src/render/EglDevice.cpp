#include "render/EglDevice.h"

#include <cstring>
#include <vector>

#include "platform/Backend.h"
#include "support/Log.h"
#include "support/Strings.h"

namespace livewall {
namespace {

const char* eglErrorText(EGLint error) {
    switch (error) {
        case EGL_SUCCESS: return "success";
        case EGL_NOT_INITIALIZED: return "not initialized";
        case EGL_BAD_ACCESS: return "bad access";
        case EGL_BAD_ALLOC: return "out of memory";
        case EGL_BAD_ATTRIBUTE: return "bad attribute";
        case EGL_BAD_CONFIG: return "bad config";
        case EGL_BAD_CONTEXT: return "bad context";
        case EGL_BAD_DISPLAY: return "bad display";
        case EGL_BAD_MATCH: return "bad match";
        case EGL_BAD_NATIVE_WINDOW: return "bad native window";
        case EGL_BAD_SURFACE: return "bad surface";
        case EGL_CONTEXT_LOST: return "context lost";
        default: return "unknown";
    }
}

void logEglFailure(const char* what) {
    Log::error(std::string(what) + ": " + eglErrorText(eglGetError()));
}

}  // namespace

std::unique_ptr<EglDevice> EglDevice::create(Backend& backend, bool wantAlpha) {
    std::unique_ptr<EglDevice> device(new EglDevice());

    // eglGetPlatformDisplay rather than eglGetDisplay. The old call guesses the
    // platform from the pointer, and on a machine running both X11 and Wayland
    // it guesses wrong often enough to matter — the app then gets an X11
    // display for a wl_display handle and fails at eglInitialize with something
    // unrelated-looking.
    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));

    if (getPlatformDisplay != nullptr) {
        device->display_ =
            getPlatformDisplay(backend.eglPlatform(), backend.nativeDisplay(), nullptr);
    } else {
        device->display_ = eglGetDisplay((EGLNativeDisplayType)backend.nativeDisplay());
    }

    if (device->display_ == EGL_NO_DISPLAY) {
        logEglFailure("no EGL display");
        return nullptr;
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(device->display_, &major, &minor) == EGL_FALSE) {
        logEglFailure("eglInitialize failed");
        return nullptr;
    }
    Log::info(format("EGL %d.%d on %s", major, minor, backend.name()));

    if (const char* extensions = eglQueryString(device->display_, EGL_EXTENSIONS);
        extensions != nullptr) {
        device->extensions_ = extensions;
    }

    if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE) {
        logEglFailure("eglBindAPI(GLES) failed");
        return nullptr;
    }

    if (!device->chooseConfig(wantAlpha)) return nullptr;
    if (!device->createContext()) return nullptr;

    device->loadExtensions();
    return device;
}

EglDevice::~EglDevice() {
    if (display_ == EGL_NO_DISPLAY) return;
    releaseCurrent();
    if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
    eglTerminate(display_);
}

bool EglDevice::chooseConfig(bool wantAlpha) {
    // Tried in order. An 8-bit alpha channel is what makes "no wallpaper of
    // ours" degrade to whatever is behind rather than to a black rectangle —
    // the same property the macOS port gets from every layer being transparent
    // — but it is worth nothing without a compositor, and asking for it on a
    // driver that has no ARGB config would fail outright.
    const EGLint alphaBits = wantAlpha ? 8 : 0;

    const EGLint attributes[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      alphaBits,
        // No depth or stencil. Everything drawn here is one full-screen
        // triangle; a depth buffer would be a per-output allocation the size of
        // the panel that nothing ever reads.
        EGL_DEPTH_SIZE,      0,
        EGL_STENCIL_SIZE,    0,
        EGL_NONE};

    EGLint count = 0;
    if (eglChooseConfig(display_, attributes, nullptr, 0, &count) == EGL_FALSE || count == 0) {
        logEglFailure("no usable EGL config");
        return false;
    }

    std::vector<EGLConfig> candidates(static_cast<size_t>(count));
    eglChooseConfig(display_, attributes, candidates.data(), count, &count);

    // eglChooseConfig sorts by its own rules, and those rules rank a config
    // with *more* alpha bits than requested above one with exactly the number
    // asked for. Asking for 0 and taking the first match therefore lands on an
    // ARGB config on most drivers, which then renders the wallpaper through an
    // alpha channel nothing composites. Pick explicitly.
    for (EGLConfig candidate : candidates) {
        EGLint alpha = 0;
        eglGetConfigAttrib(display_, candidate, EGL_ALPHA_SIZE, &alpha);
        if ((alphaBits > 0) != (alpha > 0)) continue;

        config_ = candidate;
        hasAlpha_ = alpha > 0;
        EGLint visual = 0;
        eglGetConfigAttrib(display_, candidate, EGL_NATIVE_VISUAL_ID, &visual);
        visualId_ = static_cast<unsigned>(visual);
        return true;
    }

    // Nothing matched exactly: take the first and record what it actually is,
    // so the render code can decide whether to draw opaque letterbox bars.
    config_ = candidates.front();
    EGLint alpha = 0;
    eglGetConfigAttrib(display_, config_, EGL_ALPHA_SIZE, &alpha);
    hasAlpha_ = alpha > 0;
    EGLint visual = 0;
    eglGetConfigAttrib(display_, config_, EGL_NATIVE_VISUAL_ID, &visual);
    visualId_ = static_cast<unsigned>(visual);
    return true;
}

bool EglDevice::createContext() {
    // GLES 3 first: the video path's 10-bit sampling wants the integer texture
    // formats and `textureSize()`, and GLES 2 has neither. Falling back is not
    // a loss of function — the 8-bit path is identical — so it is not worth
    // refusing to start over.
    for (const EGLint version : {3, 2}) {
        const EGLint attributes[] = {EGL_CONTEXT_CLIENT_VERSION, version, EGL_NONE};
        context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, attributes);
        if (context_ != EGL_NO_CONTEXT) {
            Log::info(format("GLES %d context", version));
            return true;
        }
    }
    logEglFailure("could not create a GLES context");
    return false;
}

void EglDevice::loadExtensions() {
    if (hasExtension("EGL_EXT_image_dma_buf_import")) {
        createImage_ =
            reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        destroyImage_ =
            reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        imageTargetTexture_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));

        // All three or none. A driver that advertises the extension but hides
        // one of the entry points would otherwise crash on the first frame
        // rather than fall back.
        if (createImage_ == nullptr || destroyImage_ == nullptr || imageTargetTexture_ == nullptr) {
            createImage_ = nullptr;
            destroyImage_ = nullptr;
            imageTargetTexture_ = nullptr;
            Log::info("dmabuf import advertised but incomplete — using the upload path");
        }
    } else {
        Log::info("no EGL_EXT_image_dma_buf_import — decoded frames will be uploaded, not mapped");
    }

    dmaBufModifiers_ = hasExtension("EGL_EXT_image_dma_buf_import_modifiers");
}

bool EglDevice::hasExtension(const char* name) const {
    // Substring search with boundary checks: "EGL_EXT_image_dma_buf_import" is
    // a prefix of "EGL_EXT_image_dma_buf_import_modifiers", and a plain find()
    // would report the first as present on a driver that only has the second.
    const size_t length = std::strlen(name);
    size_t position = extensions_.find(name);
    while (position != std::string::npos) {
        const bool startOk = position == 0 || extensions_[position - 1] == ' ';
        const size_t after = position + length;
        const bool endOk = after == extensions_.size() || extensions_[after] == ' ';
        if (startOk && endOk) return true;
        position = extensions_.find(name, position + 1);
    }
    return false;
}

EGLSurface EglDevice::createWindowSurface(std::uintptr_t nativeWindow) {
    // A C-style cast because EGLNativeWindowType is a pointer on one platform
    // and an integer on the other, and no single C++ cast covers both.
    EGLSurface surface = eglCreateWindowSurface(
        display_, config_, (EGLNativeWindowType)nativeWindow, nullptr);
    if (surface == EGL_NO_SURFACE) logEglFailure("eglCreateWindowSurface failed");
    return surface;
}

void EglDevice::destroySurface(EGLSurface surface) {
    if (surface == EGL_NO_SURFACE) return;
    if (current_ == surface) releaseCurrent();
    eglDestroySurface(display_, surface);
}

bool EglDevice::makeCurrent(EGLSurface surface) {
    if (current_ == surface) return true;
    if (eglMakeCurrent(display_, surface, surface, context_) == EGL_FALSE) {
        logEglFailure("eglMakeCurrent failed");
        return false;
    }
    current_ = surface;
    return true;
}

void EglDevice::releaseCurrent() {
    if (current_ == EGL_NO_SURFACE) return;
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    current_ = EGL_NO_SURFACE;
}

void EglDevice::setSwapInterval(int interval) { eglSwapInterval(display_, interval); }

EGLImageKHR EglDevice::createImage(const EGLint* attributes) {
    if (createImage_ == nullptr) return EGL_NO_IMAGE_KHR;
    return createImage_(display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes);
}

void EglDevice::destroyImage(EGLImageKHR image) {
    if (destroyImage_ == nullptr || image == EGL_NO_IMAGE_KHR) return;
    destroyImage_(display_, image);
}

void EglDevice::bindImageToTexture(GLenum target, EGLImageKHR image) {
    if (imageTargetTexture_ == nullptr) return;
    imageTargetTexture_(target, image);
}

}  // namespace livewall
