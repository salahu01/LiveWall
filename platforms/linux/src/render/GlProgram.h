// A compiled shader program and the full-screen triangle it draws.
//
// Compiled once per program and shared by every output, because there is one
// GL context. That matters more than it looks: the macOS port measured runtime
// shader compilation at ~97 MB of resident graphics memory that is never
// released, which was more than the rest of the app combined. Mesa's compiler
// is not as expensive as that, but compiling the same shader once per monitor
// would be paying it twice for nothing.
#pragma once

#include <GLES2/gl2.h>

#include <string>

#include "render/FitMode.h"

namespace livewall {

class GlProgram {
public:
    GlProgram() = default;
    ~GlProgram();

    GlProgram(const GlProgram&) = delete;
    GlProgram& operator=(const GlProgram&) = delete;

    // `label` names the program in log lines when compilation fails, which is
    // the only time anyone reads it.
    bool build(const char* label, const char* vertexSource, const char* fragmentSource);

    bool valid() const { return program_ != 0; }

    void use();

    void setFit(const FitTransform& transform);
    void setTime(float seconds);
    void setResolution(int width, int height);
    void setTextureUnit(int unit);

    // Binds the shared vertex buffer and issues the draw. Assumes `use()` has
    // been called.
    void drawFullScreen();

private:
    GLuint compile(GLenum type, const char* source, const char* label);
    void bindVertexBuffer();

    GLuint program_ = 0;
    GLuint vertexBuffer_ = 0;
    GLint positionAttribute_ = -1;
    GLint fitUniform_ = -1;
    GLint timeUniform_ = -1;
    GLint resolutionUniform_ = -1;
    GLint textureUniform_ = -1;
};

}  // namespace livewall
