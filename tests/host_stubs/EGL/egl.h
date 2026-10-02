#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef EGLAPIENTRY
#define EGLAPIENTRY
#endif

using EGLBoolean = unsigned int;
using EGLint = std::int32_t;
using EGLDisplay = void *;
using EGLSurface = void *;
using EGLContext = void *;
using __eglMustCastToProperFunctionPointerType = void (*)();

inline constexpr EGLBoolean EGL_FALSE = 0;
inline constexpr EGLBoolean EGL_TRUE = 1;
#define EGL_NO_CONTEXT (reinterpret_cast<EGLContext>(0))
#define EGL_NO_DISPLAY (reinterpret_cast<EGLDisplay>(0))
#define EGL_NO_SURFACE (reinterpret_cast<EGLSurface>(0))

inline constexpr EGLint EGL_HEIGHT = 0x3056;
inline constexpr EGLint EGL_WIDTH = 0x3057;
inline constexpr EGLint EGL_RENDER_BUFFER = 0x3086;
inline constexpr EGLint EGL_BACK_BUFFER = 0x3084;
inline constexpr EGLint EGL_SINGLE_BUFFER = 0x3085;

EGLContext EGLAPIENTRY eglGetCurrentContext(void);
EGLBoolean EGLAPIENTRY eglQuerySurface(EGLDisplay dpy, EGLSurface surface,
                                       EGLint attribute, EGLint *value);
EGLBoolean EGLAPIENTRY eglSwapBuffers(EGLDisplay dpy, EGLSurface surface);
__eglMustCastToProperFunctionPointerType EGLAPIENTRY
eglGetProcAddress(const char *procname);

#ifdef __cplusplus
}
#endif
