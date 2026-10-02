#pragma once

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLint = int;
using GLsizei = int;
using GLuint = unsigned int;
using GLfloat = float;
using GLchar = char;
using GLsizeiptr = std::ptrdiff_t;

inline constexpr GLboolean GL_FALSE = 0;
inline constexpr GLboolean GL_TRUE = 1;
inline constexpr GLenum GL_NO_ERROR = 0;

inline constexpr GLenum GL_DEPTH_TEST = 0x0B71;
inline constexpr GLenum GL_STENCIL_TEST = 0x0B90;
inline constexpr GLenum GL_CULL_FACE = 0x0B44;
inline constexpr GLenum GL_BLEND = 0x0BE2;
inline constexpr GLenum GL_SCISSOR_TEST = 0x0C11;

inline constexpr GLenum GL_VIEWPORT = 0x0BA2;
inline constexpr GLenum GL_SCISSOR_BOX = 0x0C10;
inline constexpr GLenum GL_COLOR_WRITEMASK = 0x0C23;

inline constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
inline constexpr GLenum GL_TEXTURE_BINDING_2D = 0x8069;
inline constexpr GLenum GL_TEXTURE0 = 0x84C0;
inline constexpr GLenum GL_TEXTURE1 = 0x84C1;
inline constexpr GLenum GL_ACTIVE_TEXTURE = 0x84E0;

inline constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
inline constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
inline constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
inline constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
inline constexpr GLenum GL_LINEAR = 0x2601;
inline constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;

inline constexpr GLenum GL_RGBA = 0x1908;
inline constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
inline constexpr GLenum GL_FLOAT = 0x1406;

inline constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
inline constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
inline constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
inline constexpr GLenum GL_LINK_STATUS = 0x8B82;
inline constexpr GLenum GL_CURRENT_PROGRAM = 0x8B8D;

inline constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
inline constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
inline constexpr GLenum GL_ARRAY_BUFFER_BINDING = 0x8894;
inline constexpr GLenum GL_ELEMENT_ARRAY_BUFFER_BINDING = 0x8895;
inline constexpr GLenum GL_STATIC_DRAW = 0x88E4;

inline constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
inline constexpr GLenum GL_FRAMEBUFFER_BINDING = 0x8CA6;

inline constexpr GLenum GL_ZERO = 0;
inline constexpr GLenum GL_ONE = 1;
inline constexpr GLenum GL_SRC_ALPHA = 0x0302;
inline constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
inline constexpr GLenum GL_BLEND_DST_RGB = 0x80C8;
inline constexpr GLenum GL_BLEND_SRC_RGB = 0x80C9;
inline constexpr GLenum GL_BLEND_DST_ALPHA = 0x80CA;
inline constexpr GLenum GL_BLEND_SRC_ALPHA = 0x80CB;

inline constexpr GLenum GL_TRIANGLE_STRIP = 0x0005;

GLenum glGetError(void);
void glGetIntegerv(GLenum pname, GLint *data);
void glGetBooleanv(GLenum pname, GLboolean *data);
GLboolean glIsEnabled(GLenum cap);
void glEnable(GLenum cap);
void glDisable(GLenum cap);
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
void glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
void glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB,
                         GLenum sfactorAlpha, GLenum dfactorAlpha);

void glActiveTexture(GLenum texture);
void glGenTextures(GLsizei n, GLuint *textures);
void glDeleteTextures(GLsizei n, const GLuint *textures);
void glBindTexture(GLenum target, GLuint texture);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glTexImage2D(GLenum target, GLint level, GLint internalformat,
                  GLsizei width, GLsizei height, GLint border, GLenum format,
                  GLenum type, const void *pixels);
void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset,
                         GLint yoffset, GLint x, GLint y, GLsizei width,
                         GLsizei height);

GLuint glCreateShader(GLenum type);
void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string,
                    const GLint *length);
void glCompileShader(GLuint shader);
void glGetShaderiv(GLuint shader, GLenum pname, GLint *params);
void glDeleteShader(GLuint shader);

GLuint glCreateProgram(void);
void glAttachShader(GLuint program, GLuint shader);
void glBindAttribLocation(GLuint program, GLuint index, const GLchar *name);
void glLinkProgram(GLuint program);
void glGetProgramiv(GLuint program, GLenum pname, GLint *params);
void glUseProgram(GLuint program);
void glDeleteProgram(GLuint program);

GLint glGetAttribLocation(GLuint program, const GLchar *name);
GLint glGetUniformLocation(GLuint program, const GLchar *name);
void glUniform1i(GLint location, GLint v0);
void glUniform1f(GLint location, GLfloat v0);
void glUniform2f(GLint location, GLfloat v0, GLfloat v1);

void glGenBuffers(GLsizei n, GLuint *buffers);
void glDeleteBuffers(GLsizei n, const GLuint *buffers);
void glBindBuffer(GLenum target, GLuint buffer);
void glBufferData(GLenum target, GLsizeiptr size, const void *data,
                  GLenum usage);

void glBindFramebuffer(GLenum target, GLuint framebuffer);

void glEnableVertexAttribArray(GLuint index);
void glDisableVertexAttribArray(GLuint index);
void glVertexAttribPointer(GLuint index, GLint size, GLenum type,
                           GLboolean normalized, GLsizei stride,
                           const void *pointer);
void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glFlush(void);

#ifdef __cplusplus
}
#endif
