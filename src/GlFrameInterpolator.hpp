#pragma once

#include "CameraSmoother.hpp"
#include "FrameGenConfig.hpp"
#include "FramePacer.hpp"

#include <atomic>
#include <cstdint>
#include <functional>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <pl/memory/Hook.hpp>

namespace framegen {

class GlStateGuard {
public:
    GlStateGuard() noexcept;
    ~GlStateGuard() noexcept;

    GlStateGuard(const GlStateGuard &) = delete;
    GlStateGuard &operator=(const GlStateGuard &) = delete;

private:
    GLint mProgram = 0;
    GLint mActiveTexture = GL_TEXTURE0;
    GLint mViewport[4]{};
    GLint mScissorBox[4]{};
    GLint mFramebuffer = 0;
    GLint mArrayBuffer = 0;
    GLint mElementArrayBuffer = 0;
    GLint mVertexArray = 0;
    GLint mTex0 = 0;
    GLint mTex1 = 0;
    GLint mBlendSrcRgb = GL_ONE;
    GLint mBlendDstRgb = GL_ZERO;
    GLint mBlendSrcAlpha = GL_ONE;
    GLint mBlendDstAlpha = GL_ZERO;

    GLboolean mDepthTest = GL_FALSE;
    GLboolean mBlend = GL_FALSE;
    GLboolean mCullFace = GL_FALSE;
    GLboolean mScissorTest = GL_FALSE;
    GLboolean mStencilTest = GL_FALSE;
    GLboolean mColorMask[4]{GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
};

class GlFrameInterpolator {
public:
    using EglSwapBuffersFn = EGLBoolean(EGLAPIENTRY *)(EGLDisplay, EGLSurface);
    using EglSwapIntervalFn = EGLBoolean(EGLAPIENTRY *)(EGLDisplay, EGLint);
    using EglPresentationTimeFn =
        EGLBoolean(EGLAPIENTRY *)(EGLDisplay, EGLSurface, std::int64_t);
    using PostFrameCallback =
        std::function<void(const RuntimeTelemetrySnapshot &)>;

    static GlFrameInterpolator &instance() noexcept;

    bool installHook() noexcept;
    void uninstallHook() noexcept;

    void setModuleEnabled(bool enabled) noexcept;
    [[nodiscard]] bool isModuleEnabled() const noexcept {
        return mModuleEnabled.load(std::memory_order_relaxed);
    }

    void updateConfig(const FrameGenConfig &config) noexcept;
    [[nodiscard]] FrameGenConfig currentConfig() const noexcept;

    void requestTemporalReset() noexcept {
        mResetRequested.store(true, std::memory_order_relaxed);
    }

    void setPostFrameCallback(PostFrameCallback callback) noexcept;

    [[nodiscard]] RuntimeTelemetrySnapshot telemetrySnapshot() const noexcept;

    // Main swap handler invoked by the eglSwapBuffers detour (and directly by host tests)
    EGLBoolean onSwapBuffers(EGLDisplay dpy, EGLSurface surface) noexcept;

    static EGLBoolean EGLAPIENTRY swapBuffersDetour(
        EGLDisplay dpy, EGLSurface surface) noexcept;

    // Test hooks for host simulation without a physical Android GPU
    void setSwapCallbacksForTest(
        EglSwapBuffersFn origSwap,
        EglPresentationTimeFn presentationTimeFn = nullptr) noexcept;
    void setInbuiltFpsCounterForTest(
        std::atomic<int> *frameCountPtr,
        std::atomic<int> *currentFpsPtr = nullptr) noexcept;
    [[nodiscard]] static std::atomic<int> *decodeArm64InbuiltFpsFrameCount(
        const void *nativeGetFpsAddr) noexcept;

private:
    GlFrameInterpolator() = default;

    void tryResolveInbuiltFpsCounter() noexcept;
    bool ensureGlResources(GLsizei width, GLsizei height) noexcept;
    void destroyGlResources() noexcept;
    bool compileSynthesisProgram() noexcept;
    bool captureBackbufferToTexture(GLuint targetTex) noexcept;
    bool drawSynthesizedQuad(float mvU, float mvV, float phase,
                             float blendWeight, float hudProtection) noexcept;
    void resetTemporalState() noexcept;

    mutable std::mutex mConfigMutex;
    FrameGenConfig mConfig{};
    PostFrameCallback mPostFrameCallback{};

    mutable std::mutex mTelemetryMutex;
    RuntimeTelemetrySnapshot mTelemetry{};

    FramePacer mFramePacer{};

    std::atomic<bool> mModuleEnabled{true};
    std::atomic<bool> mHookInstalled{false};
    std::atomic<bool> mPresentationTimeSupported{false};
    std::atomic<bool> mResetRequested{false};

    pl::memory::HookHandle mSwapHook;
    EglSwapBuffersFn mOrigSwapBuffers = nullptr;
    EglSwapIntervalFn mSwapIntervalFn = nullptr;
    EglPresentationTimeFn mPresentationTimeFn = nullptr;
    EGLSurface mLastUncappedSurface = EGL_NO_SURFACE;

    // OpenGL ES state (owned by the render thread)
    bool mGlInitialized = false;
    bool mHasFrameHistory = false;
    GLsizei mSurfaceWidth = 0;
    GLsizei mSurfaceHeight = 0;

    GLuint mTexPrev = 0;
    GLuint mTexCur = 0;
    GLuint mSynthesisProgram = 0;
    GLuint mQuadVbo = 0;

    GLint mAttrPos = -1;
    GLint mAttrUV = -1;
    GLint mUniPrevTex = -1;
    GLint mUniCurTex = -1;
    GLint mUniMotionVec = -1;
    GLint mUniPhase = -1;
    GLint mUniBlendWeight = -1;
    GLint mUniHudProtection = -1;
    GLint mUniMotionBlur = -1;
    bool mMotionBlurEnabled = false;

    std::int64_t mLastPresentedTimestampNs = 0;
    std::uint64_t mSceneCutCount = 0;
    std::uint32_t mConsecutiveGlErrors = 0;
    std::uint32_t mStationaryFrameStreak = 0;

    std::atomic<int> *mInbuiltFpsFrameCount = nullptr;
    std::atomic<int> *mInbuiltCurrentFps = nullptr;
    std::uint32_t mInbuiltFpsResolveCooldown = 0;
};

} // namespace framegen
