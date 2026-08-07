// The EGL display, config and context, shared by every output.
//
// One context for all surfaces rather than one each. The surfaces are created
// from one config and drawn from one thread, so separate contexts would buy
// nothing and cost a context switch per output per frame — and the shader
// programs and textures would have to be uploaded once per output instead of
// once.
//
// GLES rather than desktop GL. Not for portability to phones: the sampler
// extension the video path needs — GL_OES_EGL_image_external — is a GLES
// extension, and it is how a VA-API frame becomes a texture without a copy.
// Desktop GL drivers expose it inconsistently and Mesa's GLES path is the one
// that is actually tested against dmabuf import.
#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <cstdint>
#include <memory>
#include <string>

namespace livewall {

class Backend;

class EglDevice {
public:
    // `wantAlpha` asks for a config with an alpha channel, which the X11
    // backend only wants when a compositor is running. Returns null on any
    // failure, having logged what.
    static std::unique_ptr<EglDevice> create(Backend& backend, bool wantAlpha);

    ~EglDevice();

    EglDevice(const EglDevice&) = delete;
    EglDevice& operator=(const EglDevice&) = delete;

    EGLDisplay display() const { return display_; }
    EGLConfig config() const { return config_; }
    EGLContext context() const { return context_; }

    // The X visual the chosen config draws into. Zero on Wayland, where there
    // is no such thing.
    unsigned nativeVisualId() const { return visualId_; }

    bool hasAlpha() const { return hasAlpha_; }

    // The native window as an integer rather than an EGLNativeWindowType.
    //
    // That type is decided at *compile* time by which platform macro is set
    // when <EGL/eglplatform.h> is first included — `Window` (an XID, so an
    // integer) with X11, `struct wl_egl_window*` with Wayland. Two translation
    // units in this build see it differently, because one of them includes
    // wayland-egl.h first, and the result is two incompatible declarations of
    // this function and a link error naming a type that appears nowhere in the
    // source. A uintptr_t is wide enough for either and identical at the ABI
    // level, which is all eglCreateWindowSurface actually cares about.
    EGLSurface createWindowSurface(std::uintptr_t nativeWindow);
    void destroySurface(EGLSurface surface);

    bool makeCurrent(EGLSurface surface);
    void releaseCurrent();

    // Turns off the driver's vsync wait. The app paces itself — one frame per
    // tick at the wallpaper's own frame rate, which is a quarter or a fifth of
    // the refresh — and a swap interval of 1 would block the single event loop
    // in eglSwapBuffers until the next vblank, stalling the control socket, the
    // bus and every other output behind it.
    void setSwapInterval(int interval);

    // --- dmabuf import, for VideoSource -------------------------------------
    //
    // Present only when the driver advertises EGL_EXT_image_dma_buf_import.
    // Without it the video path falls back to uploading decoded frames with
    // glTexSubImage2D, which works everywhere and costs a copy per frame.
    bool supportsDmaBufImport() const { return createImage_ != nullptr; }
    bool supportsDmaBufModifiers() const { return dmaBufModifiers_; }

    EGLImageKHR createImage(const EGLint* attributes);
    void destroyImage(EGLImageKHR image);
    void bindImageToTexture(GLenum target, EGLImageKHR image);

    bool hasExtension(const char* name) const;

private:
    EglDevice() = default;

    bool chooseConfig(bool wantAlpha);
    bool createContext();
    void loadExtensions();

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface current_ = EGL_NO_SURFACE;
    unsigned visualId_ = 0;
    bool hasAlpha_ = false;
    bool dmaBufModifiers_ = false;
    std::string extensions_;

    PFNEGLCREATEIMAGEKHRPROC createImage_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage_ = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture_ = nullptr;
};

}  // namespace livewall
