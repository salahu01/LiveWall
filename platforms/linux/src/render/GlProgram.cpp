#include "render/GlProgram.h"

#include <vector>

#include "support/Log.h"

namespace livewall {
namespace {

// One triangle that covers the whole clip volume. The two vertices outside
// -1..1 are clipped away, and what is left is exactly the screen — with no
// diagonal seam, unlike the two-triangle quad it replaces.
constexpr GLfloat kFullScreenTriangle[] = {
    -1.0f, -1.0f,
     3.0f, -1.0f,
    -1.0f,  3.0f,
};

}  // namespace

GlProgram::~GlProgram() {
    // No context to delete from during static teardown; the process exit takes
    // the driver's objects with it. Deleting here matters only for the
    // wallpaper-switch case, where the old program really is dropped.
    if (vertexBuffer_ != 0) glDeleteBuffers(1, &vertexBuffer_);
    if (program_ != 0) glDeleteProgram(program_);
}

GLuint GlProgram::compile(GLenum type, const char* source, const char* label) {
    const GLuint shader = glCreateShader(type);
    if (shader == 0) return 0;

    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return shader;

    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::vector<char> message(static_cast<size_t>(length > 0 ? length : 1));
    glGetShaderInfoLog(shader, length, nullptr, message.data());
    Log::error(std::string(label) + " shader: " + message.data());

    glDeleteShader(shader);
    return 0;
}

bool GlProgram::build(const char* label, const char* vertexSource, const char* fragmentSource) {
    const GLuint vertex = compile(GL_VERTEX_SHADER, vertexSource, label);
    if (vertex == 0) return false;

    const GLuint fragment = compile(GL_FRAGMENT_SHADER, fragmentSource, label);
    if (fragment == 0) {
        glDeleteShader(vertex);
        return false;
    }

    program_ = glCreateProgram();
    glAttachShader(program_, vertex);
    glAttachShader(program_, fragment);
    glLinkProgram(program_);

    // Detached and deleted immediately: the linked program holds what it needs,
    // and leaving them attached keeps the source and the IR alive in the driver
    // for the life of the program.
    glDetachShader(program_, vertex);
    glDetachShader(program_, fragment);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        GLint length = 0;
        glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> message(static_cast<size_t>(length > 0 ? length : 1));
        glGetProgramInfoLog(program_, length, nullptr, message.data());
        Log::error(std::string(label) + " link: " + message.data());
        glDeleteProgram(program_);
        program_ = 0;
        return false;
    }

    positionAttribute_ = glGetAttribLocation(program_, "aPosition");
    fitUniform_ = glGetUniformLocation(program_, "uFit");
    timeUniform_ = glGetUniformLocation(program_, "uTime");
    resolutionUniform_ = glGetUniformLocation(program_, "uResolution");
    textureUniform_ = glGetUniformLocation(program_, "uTexture");

    bindVertexBuffer();
    return true;
}

void GlProgram::bindVertexBuffer() {
    glGenBuffers(1, &vertexBuffer_);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kFullScreenTriangle), kFullScreenTriangle,
                 GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GlProgram::use() {
    if (program_ != 0) glUseProgram(program_);
}

void GlProgram::setFit(const FitTransform& transform) {
    if (fitUniform_ < 0) return;
    glUniform4f(fitUniform_, transform.scaleX, transform.scaleY, transform.offsetX,
                transform.offsetY);
}

void GlProgram::setTime(float seconds) {
    if (timeUniform_ >= 0) glUniform1f(timeUniform_, seconds);
}

void GlProgram::setResolution(int width, int height) {
    if (resolutionUniform_ >= 0) {
        glUniform2f(resolutionUniform_, static_cast<float>(width), static_cast<float>(height));
    }
}

void GlProgram::setTextureUnit(int unit) {
    if (textureUniform_ >= 0) glUniform1i(textureUniform_, unit);
}

void GlProgram::drawFullScreen() {
    if (program_ == 0 || positionAttribute_ < 0) return;

    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer_);
    glEnableVertexAttribArray(static_cast<GLuint>(positionAttribute_));
    glVertexAttribPointer(static_cast<GLuint>(positionAttribute_), 2, GL_FLOAT, GL_FALSE, 0,
                          nullptr);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableVertexAttribArray(static_cast<GLuint>(positionAttribute_));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

}  // namespace livewall
