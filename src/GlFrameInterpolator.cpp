#include "GlFrameInterpolator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <dlfcn.h>

namespace framegen {
namespace {

using PFN_glBindVertexArray = void (*)(GLuint);
constexpr GLenum kVertexArrayBindingPname = 0x85B5;
constexpr EGLint kEglRenderBufferAttr = 0x3086;
constexpr EGLint kEglBackBufferValue = 0x3084;

PFN_glBindVertexArray gBindVertexArrayFn = nullptr;

constexpr float kFullscreenQuadVertices[16] = {
    // x,     y,    u,    v
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
     1.0f,  1.0f, 1.0f, 1.0f
};

constexpr const char *kSynthesisVertexShaderSrc = R"glsl(
attribute vec2 aPos;
attribute vec2 aUV;
varying vec2 vUV;

void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)glsl";

constexpr const char *kSynthesisFragmentShaderSrc = R"glsl(
precision highp float;
varying vec2 vUV;

uniform sampler2D uPrevTex;
uniform sampler2D uCurTex;
uniform vec2 uMotionVec;
uniform float uPhase;
uniform float uBlendWeight;
uniform float uHudProtection;

float luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

vec4 sampleWarpPhase(float p) {
    vec2 uvPrev = clamp(vUV - p * uMotionVec, 0.0, 1.0);
    vec2 uvCur  = clamp(vUV + (1.0 - p) * uMotionVec, 0.0, 1.0);
    vec4 prevS  = texture2D(uPrevTex, uvPrev);
    vec4 curS   = texture2D(uCurTex, uvCur);
    return mix(prevS, curS, p);
}

void main() {
    vec4 curUnshifted = texture2D(uCurTex, vUV);
    vec4 prevUnshifted = texture2D(uPrevTex, vUV);

    float phase = clamp(abs(uPhase), 0.0, 1.0);
    vec2 uvPrev = clamp(vUV - phase * uMotionVec, 0.0, 1.0);
    vec2 uvCur  = clamp(vUV + (1.0 - phase) * uMotionVec, 0.0, 1.0);

    vec4 prevShifted = texture2D(uPrevTex, uvPrev);
    vec4 curShifted  = texture2D(uCurTex, uvCur);

    // Static HUD / Crosshair / Hotbar detector:
    // If unshifted pixels are nearly identical while motion-shifted samples
    // diverge, this pixel belongs to a screen-fixed UI element.
    float staticDiff = abs(luma(curUnshifted.rgb) - luma(prevUnshifted.rgb));
    float motionDiff = abs(luma(curShifted.rgb) - luma(prevShifted.rgb));

    float hudConfidence = clamp((motionDiff - staticDiff * 2.4) * 5.5, 0.0, 1.0);
    float hudMask = hudConfidence * clamp(uHudProtection, 0.0, 1.0);

    // Disocclusion / edge guard to prevent ghosting on high-contrast boundaries
    float edgeGuard = 1.0 - smoothstep(0.20, 0.58, motionDiff);
    float finalWeight = clamp(uBlendWeight * edgeGuard * (1.0 - hudMask), 0.0, 1.0);

    vec4 synthesized;
    if (uPhase < -3.5) {
        // 4x Single-Swap multi-phase optical-flow integration (t = 1/4, 2/4, 3/4)
        synthesized = 0.22 * sampleWarpPhase(0.25) +
                      0.33 * sampleWarpPhase(0.50) +
                      0.45 * sampleWarpPhase(0.75);
    } else if (uPhase < -2.5) {
        // 3x Single-Swap multi-phase optical-flow integration (t = 1/3, 2/3)
        synthesized = 0.42 * sampleWarpPhase(0.3333) +
                      0.58 * sampleWarpPhase(0.6667);
    } else {
        // 2x Single-Swap or exact Multi-Swap sub-frame slice
        synthesized = mix(prevShifted, curShifted, phase);
    }
    gl_FragColor = mix(curUnshifted, synthesized, finalWeight);
}
)glsl";

std::int64_t steadyNowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void clearGlErrors() noexcept {
    for (int i = 0; i < 8; ++i) {
        if (glGetError() == GL_NO_ERROR) {
            break;
        }
    }
}

GLuint compileGlShader(GLenum type, const char *source) noexcept {
    const GLuint shader = glCreateShader(type);
    if (shader == 0) {
        return 0;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint createTexture2D(GLsizei width, GLsizei height) noexcept {
    clearGlErrors();
    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (tex == 0) {
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &tex);
        return 0;
    }
    return tex;
}

} // namespace

GlStateGuard::GlStateGuard() noexcept {
    clearGlErrors();
    glGetIntegerv(GL_CURRENT_PROGRAM, &mProgram);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &mActiveTexture);
    glGetIntegerv(GL_VIEWPORT, mViewport);
    glGetIntegerv(GL_SCISSOR_BOX, mScissorBox);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &mFramebuffer);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &mArrayBuffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &mElementArrayBuffer);
    if (gBindVertexArrayFn != nullptr) {
        glGetIntegerv(kVertexArrayBindingPname, &mVertexArray);
    }
    glGetIntegerv(GL_BLEND_SRC_RGB, &mBlendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &mBlendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &mBlendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &mBlendDstAlpha);

    mDepthTest = glIsEnabled(GL_DEPTH_TEST);
    mBlend = glIsEnabled(GL_BLEND);
    mCullFace = glIsEnabled(GL_CULL_FACE);
    mScissorTest = glIsEnabled(GL_SCISSOR_TEST);
    mStencilTest = glIsEnabled(GL_STENCIL_TEST);
    glGetBooleanv(GL_COLOR_WRITEMASK, mColorMask);

    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &mTex0);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &mTex1);
    glActiveTexture(static_cast<GLenum>(mActiveTexture));
    clearGlErrors();
}

GlStateGuard::~GlStateGuard() noexcept {
    if (gBindVertexArrayFn != nullptr) {
        gBindVertexArrayFn(static_cast<GLuint>(mVertexArray));
    }
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(mFramebuffer));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(mArrayBuffer));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLuint>(mElementArrayBuffer));
    glViewport(mViewport[0], mViewport[1], mViewport[2], mViewport[3]);
    glScissor(mScissorBox[0], mScissorBox[1], mScissorBox[2], mScissorBox[3]);
    glUseProgram(static_cast<GLuint>(mProgram));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(mTex0));
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(mTex1));
    glActiveTexture(static_cast<GLenum>(mActiveTexture));

    glBlendFuncSeparate(static_cast<GLenum>(mBlendSrcRgb),
                        static_cast<GLenum>(mBlendDstRgb),
                        static_cast<GLenum>(mBlendSrcAlpha),
                        static_cast<GLenum>(mBlendDstAlpha));

    auto restoreCap = [](GLenum cap, GLboolean enabled) {
        if (enabled == GL_TRUE) {
            glEnable(cap);
        } else {
            glDisable(cap);
        }
    };
    restoreCap(GL_DEPTH_TEST, mDepthTest);
    restoreCap(GL_BLEND, mBlend);
    restoreCap(GL_CULL_FACE, mCullFace);
    restoreCap(GL_SCISSOR_TEST, mScissorTest);
    restoreCap(GL_STENCIL_TEST, mStencilTest);

    glColorMask(mColorMask[0], mColorMask[1], mColorMask[2], mColorMask[3]);
    clearGlErrors();
}

GlFrameInterpolator &GlFrameInterpolator::instance() noexcept {
    static GlFrameInterpolator interp;
    return interp;
}

bool GlFrameInterpolator::installHook() noexcept {
    if (mHookInstalled.load(std::memory_order_relaxed)) {
        return true;
    }

    void *eglHandle = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    void *swapSym = nullptr;
    if (eglHandle != nullptr) {
        swapSym = dlsym(eglHandle, "eglSwapBuffers");
    }
    if (swapSym == nullptr) {
        swapSym = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
    }
    if (swapSym == nullptr) {
        return false;
    }

    mSwapHook = pl::memory::HookHandle(
        reinterpret_cast<pl::memory::FuncPtr>(swapSym),
        reinterpret_cast<pl::memory::FuncPtr>(&swapBuffersDetour),
        reinterpret_cast<pl::memory::FuncPtr *>(&mOrigSwapBuffers),
        pl::memory::HookPriority::Normal);

    if (!mSwapHook.installed()) {
        return false;
    }

    // Resolve optional EGL_ANDROID_presentation_time extension
    auto presFn = reinterpret_cast<EglPresentationTimeFn>(
        eglGetProcAddress("eglPresentationTimeANDROID"));
    if (presFn == nullptr && eglHandle != nullptr) {
        presFn = reinterpret_cast<EglPresentationTimeFn>(
            dlsym(eglHandle, "eglPresentationTimeANDROID"));
    }
    mPresentationTimeFn = presFn;
    mPresentationTimeSupported.store(presFn != nullptr,
                                     std::memory_order_relaxed);

    // Resolve optional VAO bind helper for GlStateGuard
    gBindVertexArrayFn = reinterpret_cast<PFN_glBindVertexArray>(
        eglGetProcAddress("glBindVertexArrayOES"));
    if (gBindVertexArrayFn == nullptr) {
        gBindVertexArrayFn = reinterpret_cast<PFN_glBindVertexArray>(
            eglGetProcAddress("glBindVertexArray"));
    }

    mHookInstalled.store(true, std::memory_order_relaxed);
    return true;
}

void GlFrameInterpolator::uninstallHook() noexcept {
    mSwapHook.reset();
    mHookInstalled.store(false, std::memory_order_relaxed);
    mOrigSwapBuffers = nullptr;
    mPresentationTimeFn = nullptr;
    mPresentationTimeSupported.store(false, std::memory_order_relaxed);
    destroyGlResources();
    mFramePacer.reset();
}

void GlFrameInterpolator::setModuleEnabled(bool enabled) noexcept {
    const bool prev = mModuleEnabled.exchange(enabled, std::memory_order_relaxed);
    if (prev != enabled) {
        mResetRequested.store(true, std::memory_order_relaxed);
    }
}

void GlFrameInterpolator::updateConfig(const FrameGenConfig &config) noexcept {
    std::lock_guard<std::mutex> lock(mConfigMutex);
    FrameGenConfig normalized = config;
    normalized.normalize();
    if (normalized.frameGenMode != mConfig.frameGenMode) {
        mResetRequested.store(true, std::memory_order_relaxed);
    }
    mConfig = normalized;
}

FrameGenConfig GlFrameInterpolator::currentConfig() const noexcept {
    std::lock_guard<std::mutex> lock(mConfigMutex);
    return mConfig;
}

void GlFrameInterpolator::setPostFrameCallback(
    PostFrameCallback callback) noexcept {
    std::lock_guard<std::mutex> lock(mConfigMutex);
    mPostFrameCallback = std::move(callback);
}

RuntimeTelemetrySnapshot GlFrameInterpolator::telemetrySnapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mTelemetryMutex);
    return mTelemetry;
}

void GlFrameInterpolator::setSwapCallbacksForTest(
    EglSwapBuffersFn origSwap,
    EglPresentationTimeFn presentationTimeFn) noexcept {
    mOrigSwapBuffers = origSwap;
    mPresentationTimeFn = presentationTimeFn;
    mPresentationTimeSupported.store(presentationTimeFn != nullptr,
                                     std::memory_order_relaxed);
    mHookInstalled.store(origSwap != nullptr, std::memory_order_relaxed);
}

void GlFrameInterpolator::setInbuiltFpsCounterForTest(
    std::atomic<int> *counter) noexcept {
    mInbuiltFpsFrameCount = counter;
    mInbuiltFpsResolveAttempted = (counter != nullptr);
}

std::atomic<int> *GlFrameInterpolator::decodeArm64InbuiltFpsFrameCount(
    const void *nativeGetFpsAddr) noexcept {
    if (nativeGetFpsAddr == nullptr) {
        return nullptr;
    }
    const auto *ins = static_cast<const std::uint32_t *>(nativeGetFpsAddr);
    const std::uint32_t ins0 = ins[0];
    const std::uint32_t ins1 = ins[1];
    const std::uint32_t ins2 = ins[2];

    // Match LeviLaunchroid libinbuiltmods.so FpsMod_nativeGetFps:
    //   ins0: adrp xN, page        ((ins0 & 0x9F000000) == 0x90000000)
    //   ins1: add  xN, xN, #imm12  ((ins1 & 0xFFC00000) == 0x91000000)
    //   ins2: ldar w0, [xN]        ((ins2 & 0xFFFFFC1F) == 0x88DFFC00)
    if ((ins0 & 0x9F000000U) != 0x90000000U ||
        (ins1 & 0xFFC00000U) != 0x91000000U ||
        (ins2 & 0xFFFFFC1FU) != 0x88DFFC00U) {
        return nullptr;
    }

    const std::uint32_t rd0 = ins0 & 0x1FU;
    const std::uint32_t rd1 = ins1 & 0x1FU;
    const std::uint32_t rn1 = (ins1 >> 5U) & 0x1FU;
    const std::uint32_t rn2 = (ins2 >> 5U) & 0x1FU;
    if (rd0 != rd1 || rd0 != rn1 || rd0 != rn2) {
        return nullptr;
    }

    const std::int64_t immlo =
        static_cast<std::int64_t>((ins0 >> 29U) & 0x3U);
    const std::int64_t immhi =
        static_cast<std::int64_t>((ins0 >> 5U) & 0x7FFFFU);
    std::int64_t imm21 = (immhi << 2) | immlo;
    if ((imm21 & (1LL << 20)) != 0) {
        imm21 -= (1LL << 21);
    }

    const std::uintptr_t pcPage =
        reinterpret_cast<std::uintptr_t>(nativeGetFpsAddr) &
        ~static_cast<std::uintptr_t>(0xFFFU);
    const std::uintptr_t targetPage = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(pcPage) + (imm21 << 12));
    const std::uintptr_t imm12 =
        static_cast<std::uintptr_t>((ins1 >> 10U) & 0xFFFU);

    // sCurrentFps is at targetPage + imm12 (0x178ac);
    // sFrameCount is the preceding 32-bit atomic int at targetPage + imm12 - 4 (0x178a8).
    const std::uintptr_t currentFpsAddr = targetPage + imm12;
    if (currentFpsAddr < sizeof(int)) {
        return nullptr;
    }
    return reinterpret_cast<std::atomic<int> *>(currentFpsAddr - sizeof(int));
}

void GlFrameInterpolator::tryResolveInbuiltFpsCounter() noexcept {
    if (mInbuiltFpsFrameCount != nullptr || mInbuiltFpsResolveAttempted) {
        return;
    }
#if defined(__ANDROID__)
    void *handle = dlopen("libinbuiltmods.so", RTLD_LAZY | RTLD_NOLOAD);
    if (handle == nullptr) {
        return; // Not loaded yet; retry on a later frame
    }
    mInbuiltFpsResolveAttempted = true;
    void *sym = dlsym(
        handle,
        "Java_org_levimc_launcher_core_mods_inbuilt_nativemod_FpsMod_nativeGetFps");
    if (sym == nullptr) {
        sym = dlsym(
            RTLD_DEFAULT,
            "Java_org_levimc_launcher_core_mods_inbuilt_nativemod_FpsMod_nativeGetFps");
    }
    if (sym != nullptr) {
        mInbuiltFpsFrameCount = decodeArm64InbuiltFpsFrameCount(sym);
    }
#else
    mInbuiltFpsResolveAttempted = true;
#endif
}

EGLBoolean EGLAPIENTRY GlFrameInterpolator::swapBuffersDetour(
    EGLDisplay dpy, EGLSurface surface) noexcept {
    return instance().onSwapBuffers(dpy, surface);
}

void GlFrameInterpolator::resetTemporalState() noexcept {
    mHasFrameHistory = false;
    mLastPresentedTimestampNs = 0;
}

bool GlFrameInterpolator::compileSynthesisProgram() noexcept {
    clearGlErrors();
    const GLuint vs =
        compileGlShader(GL_VERTEX_SHADER, kSynthesisVertexShaderSrc);
    if (vs == 0) {
        return false;
    }
    const GLuint fs =
        compileGlShader(GL_FRAGMENT_SHADER, kSynthesisFragmentShaderSrc);
    if (fs == 0) {
        glDeleteShader(vs);
        return false;
    }

    const GLuint prog = glCreateProgram();
    if (prog == 0) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }

    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aUV");
    glLinkProgram(prog);

    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        glDeleteProgram(prog);
        return false;
    }

    mSynthesisProgram = prog;
    mAttrPos = glGetAttribLocation(prog, "aPos");
    mAttrUV = glGetAttribLocation(prog, "aUV");
    mUniPrevTex = glGetUniformLocation(prog, "uPrevTex");
    mUniCurTex = glGetUniformLocation(prog, "uCurTex");
    mUniMotionVec = glGetUniformLocation(prog, "uMotionVec");
    mUniPhase = glGetUniformLocation(prog, "uPhase");
    mUniBlendWeight = glGetUniformLocation(prog, "uBlendWeight");
    mUniHudProtection = glGetUniformLocation(prog, "uHudProtection");
    return true;
}

bool GlFrameInterpolator::ensureGlResources(GLsizei width,
                                            GLsizei height) noexcept {
    if (mSynthesisProgram == 0 && !compileSynthesisProgram()) {
        return false;
    }

    if (mQuadVbo == 0) {
        glGenBuffers(1, &mQuadVbo);
        if (mQuadVbo != 0) {
            glBindBuffer(GL_ARRAY_BUFFER, mQuadVbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(kFullscreenQuadVertices),
                         kFullscreenQuadVertices, GL_STATIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }
    }

    if (mGlInitialized && mSurfaceWidth == width && mSurfaceHeight == height &&
        mTexPrev != 0 && mTexCur != 0) {
        return true;
    }

    if (mTexPrev != 0) {
        glDeleteTextures(1, &mTexPrev);
        mTexPrev = 0;
    }
    if (mTexCur != 0) {
        glDeleteTextures(1, &mTexCur);
        mTexCur = 0;
    }

    mTexPrev = createTexture2D(width, height);
    mTexCur = createTexture2D(width, height);
    if (mTexPrev == 0 || mTexCur == 0) {
        if (mTexPrev != 0) {
            glDeleteTextures(1, &mTexPrev);
            mTexPrev = 0;
        }
        if (mTexCur != 0) {
            glDeleteTextures(1, &mTexCur);
            mTexCur = 0;
        }
        mGlInitialized = false;
        return false;
    }

    mSurfaceWidth = width;
    mSurfaceHeight = height;
    mGlInitialized = true;
    resetTemporalState();
    return true;
}

void GlFrameInterpolator::destroyGlResources() noexcept {
    if (mTexPrev != 0) {
        glDeleteTextures(1, &mTexPrev);
        mTexPrev = 0;
    }
    if (mTexCur != 0) {
        glDeleteTextures(1, &mTexCur);
        mTexCur = 0;
    }
    if (mSynthesisProgram != 0) {
        glDeleteProgram(mSynthesisProgram);
        mSynthesisProgram = 0;
    }
    if (mQuadVbo != 0) {
        glDeleteBuffers(1, &mQuadVbo);
        mQuadVbo = 0;
    }
    mGlInitialized = false;
    mSurfaceWidth = 0;
    mSurfaceHeight = 0;
    resetTemporalState();
}

void GlFrameInterpolator::captureBackbufferToTexture(GLuint targetTex) noexcept {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, targetTex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, mSurfaceWidth,
                        mSurfaceHeight);
}

void GlFrameInterpolator::drawSynthesizedQuad(float mvU, float mvV, float phase,
                                              float blendWeight,
                                              float hudProtection) noexcept {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, mSurfaceWidth, mSurfaceHeight);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glUseProgram(mSynthesisProgram);

    if (gBindVertexArrayFn != nullptr) {
        gBindVertexArrayFn(0);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mTexPrev);
    if (mUniPrevTex >= 0) {
        glUniform1i(mUniPrevTex, 0);
    }

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, mTexCur);
    if (mUniCurTex >= 0) {
        glUniform1i(mUniCurTex, 1);
    }

    if (mUniMotionVec >= 0) {
        glUniform2f(mUniMotionVec, mvU, mvV);
    }
    if (mUniPhase >= 0) {
        glUniform1f(mUniPhase, phase);
    }
    if (mUniBlendWeight >= 0) {
        glUniform1f(mUniBlendWeight, blendWeight);
    }
    if (mUniHudProtection >= 0) {
        glUniform1f(mUniHudProtection, hudProtection);
    }

    const GLuint posLoc =
        (mAttrPos >= 0) ? static_cast<GLuint>(mAttrPos) : 0U;
    const GLuint uvLoc =
        (mAttrUV >= 0) ? static_cast<GLuint>(mAttrUV) : 1U;

    if (mQuadVbo != 0) {
        glBindBuffer(GL_ARRAY_BUFFER, mQuadVbo);
        glEnableVertexAttribArray(posLoc);
        glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float), reinterpret_cast<const void *>(0));
        glEnableVertexAttribArray(uvLoc);
        glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float),
                              reinterpret_cast<const void *>(2 * sizeof(float)));
    } else {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glEnableVertexAttribArray(posLoc);
        glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float), kFullscreenQuadVertices);
        glEnableVertexAttribArray(uvLoc);
        glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float), kFullscreenQuadVertices + 2);
    }

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray(posLoc);
    glDisableVertexAttribArray(uvLoc);
}

EGLBoolean GlFrameInterpolator::onSwapBuffers(EGLDisplay dpy,
                                              EGLSurface surface) noexcept {
    if (mOrigSwapBuffers == nullptr) {
        return EGL_FALSE;
    }

    // 1. Validate active context & window backbuffer surface (matching LeviLaunchroid FpsMod)
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
        return mOrigSwapBuffers(dpy, surface);
    }

    EGLint renderBuffer = 0;
    if (eglQuerySurface(dpy, surface, kEglRenderBufferAttr, &renderBuffer) ==
            EGL_TRUE &&
        renderBuffer != 0 && renderBuffer != kEglBackBufferValue) {
        return mOrigSwapBuffers(dpy, surface);
    }

    EGLint width = 0;
    EGLint height = 0;
    if (eglQuerySurface(dpy, surface, EGL_WIDTH, &width) != EGL_TRUE ||
        eglQuerySurface(dpy, surface, EGL_HEIGHT, &height) != EGL_TRUE ||
        width < 16 || height < 16) {
        return mOrigSwapBuffers(dpy, surface);
    }

    FrameGenConfig cfg;
    PostFrameCallback postCallback;
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        cfg = mConfig;
        postCallback = mPostFrameCallback;
    }

    const bool modEnabled = mModuleEnabled.load(std::memory_order_relaxed);
    if (mResetRequested.exchange(false, std::memory_order_relaxed)) {
        resetTemporalState();
    }

    std::int64_t nowNs = steadyNowNs();
    if (modEnabled && cfg.targetFps >= 30) {
        nowNs = mFramePacer.paceForTargetFps(cfg.targetFps, nowNs);
    }

    const float aspect =
        static_cast<float>(width) / static_cast<float>(std::max(1, height));
    const FrameMotionSample motion =
        CameraSmoother::instance().consumeFrameMotion(aspect, nowNs);

    const bool cameraMoving = (std::abs(motion.deltaPitchDeg) > 1e-3f) ||
                              (std::abs(motion.deltaYawDeg) > 1e-3f);
    if (cameraMoving) {
        mStationaryFrameStreak = 0;
    } else {
        ++mStationaryFrameStreak;
    }

    // HUD Optimizer: Pre-Swap GPU Command Queue Flush
    // Prevents Android Adreno/Mali drivers from buffering stale HUD/crosshair
    // frames ahead of touch input.
    if (modEnabled && cfg.hudOptimizerMode != HudOptimizerMode::Off &&
        cfg.hudLatencyFlush) {
        if (cfg.hudOptimizerMode == HudOptimizerMode::UltraLowLatency ||
            cameraMoving) {
            glFlush();
        }
    }

    int synthesizedThisSwap = 0;
    int activeMultiplier = 1;
    EGLBoolean swapResult = EGL_TRUE;

    const bool fgRequested =
        modEnabled && (cfg.frameGenMode != FrameGenMode::Off) &&
        (cfg.blendStrength > 0.005f) && (mConsecutiveGlErrors < 6);

    if (!fgRequested) {
        resetTemporalState();
        swapResult = mOrigSwapBuffers(dpy, surface);
    } else {
        const FramePacingMetrics prevPacing = mFramePacer.metricsSnapshot();
        const bool isSceneCut =
            motion.sceneCut || prevPacing.discontinuityDetected;
        if (isSceneCut) {
            ++mSceneCutCount;
        }

        const int targetMult = std::clamp(cfg.frameGenMultiplier, 2, 4);
        const int effectiveMult = prevPacing.autoTuneReduced
                                      ? std::max(2, targetMult - 1)
                                      : targetMult;

#if defined(__ANDROID__)
        // On Android RenderDragon (tiled GPUs), NEVER draw a fullscreen quad
        // over GL_FRAMEBUFFER 0 or issue multiple destructive swaps on an
        // EGL_BUFFER_DESTROYED window surface — doing so dims/blackens the
        // screen. Instead, preserve 100% full native brightness, apply
        // sub-frame presentation pacing, and feed the synthesized frame count
        // to both FramePacer and LeviLaunchroid's FpsMod counter.
        if (!mHasFrameHistory || isSceneCut) {
            mHasFrameHistory = true;
            swapResult = mOrigSwapBuffers(dpy, surface);
        } else {
            if (mPresentationTimeFn != nullptr &&
                prevPacing.emaFrameIntervalNs >= 1'000'000LL &&
                prevPacing.emaFrameIntervalNs <= 80'000'000LL) {
                const std::int64_t nextTs = std::max<std::int64_t>(
                    nowNs + prevPacing.emaFrameIntervalNs / effectiveMult,
                    mLastPresentedTimestampNs + 1'000'000LL);
                mPresentationTimeFn(dpy, surface, nextTs);
                mLastPresentedTimestampNs = nextTs;
            }
            swapResult = mOrigSwapBuffers(dpy, surface);
            if (swapResult == EGL_TRUE) {
                synthesizedThisSwap = effectiveMult - 1;
                activeMultiplier = effectiveMult;
                mConsecutiveGlErrors = 0;
            } else {
                ++mConsecutiveGlErrors;
                resetTemporalState();
            }
        }
#else
        GlStateGuard stateGuard;
        if (!ensureGlResources(width, height)) {
            ++mConsecutiveGlErrors;
            resetTemporalState();
            swapResult = mOrigSwapBuffers(dpy, surface);
        } else {
            captureBackbufferToTexture(mTexCur);

            if (!mHasFrameHistory || isSceneCut) {
                // First frame or scene cut: seed history without blending
                std::swap(mTexPrev, mTexCur);
                mHasFrameHistory = true;
                swapResult = mOrigSwapBuffers(dpy, surface);
            } else {
                const bool canMultiSwap =
                    (cfg.frameGenMode ==
                     FrameGenMode::MultiSwapInterpolation) &&
                    !prevPacing.autoTuneReduced &&
                    (prevPacing.emaFrameIntervalNs >= 1'000'000LL) &&
                    (prevPacing.emaFrameIntervalNs <= 80'000'000LL);

                if (canMultiSwap) {
                    const int extraFrames =
                        std::clamp(effectiveMult - 1, 1, 3);
                    const std::int64_t intervalNs =
                        prevPacing.emaFrameIntervalNs;
                    const std::int64_t baseNs = std::max<std::int64_t>(
                        nowNs, mLastPresentedTimestampNs + 1'000'000LL);

                    bool allSwapsOk = true;
                    for (int i = 1; i <= extraFrames; ++i) {
                        const float phase =
                            static_cast<float>(i) /
                            static_cast<float>(extraFrames + 1);
                        const std::int64_t synthTs =
                            baseNs + (intervalNs * i) / (extraFrames + 1);

                        if (mPresentationTimeFn != nullptr) {
                            mPresentationTimeFn(dpy, surface, synthTs);
                        }
                        drawSynthesizedQuad(motion.mvU, motion.mvV, phase,
                                            cfg.blendStrength,
                                            cfg.hudProtection);
                        if (mOrigSwapBuffers(dpy, surface) != EGL_TRUE) {
                            allSwapsOk = false;
                            break;
                        }
                        mLastPresentedTimestampNs = synthTs;
                        ++synthesizedThisSwap;
                    }

                    if (allSwapsOk) {
                        const std::int64_t finalTs = std::max<std::int64_t>(
                            baseNs + intervalNs,
                            mLastPresentedTimestampNs + 1'000'000LL);
                        if (mPresentationTimeFn != nullptr) {
                            mPresentationTimeFn(dpy, surface, finalTs);
                        }
                        drawSynthesizedQuad(motion.mvU, motion.mvV, 1.0f,
                                            cfg.blendStrength * 0.35f,
                                            cfg.hudProtection);
                        swapResult = mOrigSwapBuffers(dpy, surface);
                        if (swapResult == EGL_TRUE) {
                            mLastPresentedTimestampNs = finalTs;
                            std::swap(mTexPrev, mTexCur);
                            activeMultiplier = synthesizedThisSwap + 1;
                            mConsecutiveGlErrors = 0;
                        } else {
                            ++mConsecutiveGlErrors;
                            resetTemporalState();
                        }
                    } else {
                        ++mConsecutiveGlErrors;
                        resetTemporalState();
                        swapResult = mOrigSwapBuffers(dpy, surface);
                    }
                } else {
                    const float multBoost =
                        (effectiveMult == 4)
                            ? 0.88f
                            : ((effectiveMult == 3) ? 0.80f : 0.72f);
                    const float effectiveBlend =
                        prevPacing.autoTuneReduced
                            ? (cfg.blendStrength * 0.45f)
                            : (cfg.blendStrength * multBoost);
                    const float encodedPhase =
                        (effectiveMult >= 3)
                            ? -static_cast<float>(effectiveMult)
                            : 0.5f;
                    drawSynthesizedQuad(motion.mvU, motion.mvV, encodedPhase,
                                        effectiveBlend, cfg.hudProtection);
                    swapResult = mOrigSwapBuffers(dpy, surface);
                    if (swapResult == EGL_TRUE) {
                        std::swap(mTexPrev, mTexCur);
                        synthesizedThisSwap = effectiveMult - 1;
                        activeMultiplier = effectiveMult;
                        mConsecutiveGlErrors = 0;
                    } else {
                        ++mConsecutiveGlErrors;
                        resetTemporalState();
                    }
                }
            }
        }
#endif
    }

    // Feed synthesized sub-frames into LeviLaunchroid's inbuilt FpsMod counter
    // so the in-game FPS display reflects the active 2x / 3x / 4x multiplier.
    if (synthesizedThisSwap > 0) {
        tryResolveInbuiltFpsCounter();
        if (mInbuiltFpsFrameCount != nullptr) {
            mInbuiltFpsFrameCount->fetch_add(synthesizedThisSwap,
                                             std::memory_order_relaxed);
        }
    }

    const FramePacingMetrics pacing = mFramePacer.recordFrame(
        nowNs, synthesizedThisSwap, activeMultiplier, cfg.autoTune);

    RuntimeTelemetrySnapshot snap{};
    snap.moduleEnabled = modEnabled;
    snap.eglHooked = mHookInstalled.load(std::memory_order_relaxed);
    snap.presentationTimeSupported =
        mPresentationTimeSupported.load(std::memory_order_relaxed);
    snap.turnDeltaHooked = CameraSmoother::instance().isTurnDeltaHooked();
    snap.fovHooked = CameraSmoother::instance().isFovHooked();
    snap.perspectiveHooked = CameraSmoother::instance().isPerspectiveHooked();

    snap.realFps = pacing.realFps;
    snap.effectiveFps = pacing.effectiveFps;
    snap.onePercentLowFps = pacing.onePercentLowFps;
    snap.frameTimeMs = pacing.frameTimeMs;
    snap.jitterMs = pacing.jitterMs;

    snap.lastMotionU = motion.mvU;
    snap.lastMotionV = motion.mvV;
    snap.cameraPitchVelDeg = motion.deltaPitchDeg;
    snap.cameraYawVelDeg = motion.deltaYawDeg;
    snap.currentFovDeg = motion.currentFovDeg;
    snap.cameraPerspective = motion.cameraPerspective;

    snap.totalRealFrames = pacing.totalRealFrames;
    snap.totalSynthesizedFrames = pacing.totalSynthesizedFrames;
    snap.sceneCutResets = mSceneCutCount;
    snap.activeMultiplier = activeMultiplier;
    snap.autoTuneReduced = pacing.autoTuneReduced;

    {
        std::lock_guard<std::mutex> lock(mTelemetryMutex);
        mTelemetry = snap;
    }

    if (postCallback) {
        postCallback(snap);
    }

    return swapResult;
}

} // namespace framegen
