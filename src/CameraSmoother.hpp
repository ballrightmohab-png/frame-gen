#pragma once

#include "FrameGenConfig.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>

#include <pl/memory/Hook.hpp>

namespace framegen {

struct Vec2 {
    float pitch = 0.0f;
    float yaw = 0.0f;
};

struct FrameMotionSample {
    float mvU = 0.0f;               // Normalized horizontal optical flow [-0.25, +0.25]
    float mvV = 0.0f;               // Normalized vertical optical flow [-0.25, +0.25]
    float prevMvU = 0.0f;           // Previous frame horizontal flow (for temporal stability)
    float prevMvV = 0.0f;           // Previous frame vertical flow (for temporal stability)
    float tanHalfFovH = 0.7002f;    // tan(fovHoriz * 0.5) for 3D perspective-tangent warp
    float tanHalfFovV = 0.7002f;    // tan(fovVert * 0.5) for 3D perspective-tangent warp
    float motionConfidence = 1.0f;  // [0.0, 1.0] reliability of global camera motion
    float deltaPitchDeg = 0.0f;     // Total smoothed pitch delta this frame (deg)
    float deltaYawDeg = 0.0f;       // Total smoothed yaw delta this frame (deg)
    float angularSpeedDegPerFrame = 0.0f;
    float currentFovDeg = 70.0f;
    int cameraPerspective = 0;
    bool sceneCut = false;          // True if fast flick or perspective toggle occurred
};

class CameraSmoother {
public:
    static CameraSmoother &instance() noexcept;

    // Installs native hooks into libminecraftpe.so (applyTurnDelta, CameraAPI FOV,
    // VanillaCameraAPI perspective). Safe to call even if libminecraftpe.so is not
    // loaded or signatures do not match (falls back gracefully).
    bool installHooks() noexcept;
    void uninstallHooks() noexcept;

    // Resets all velocity reservoirs, deadzone accumulators, and frame motion state.
    void reset() noexcept;

    // Updates internal configuration snapshot used by the real-time turn hook.
    void updateConfig(const FrameGenConfig &config, bool moduleEnabled) noexcept;

    // Processes a raw camera turn delta (in degrees) at timestamp `nowNs`.
    // Public so both the native `applyTurnDelta` detour and host tests can invoke it.
    Vec2 processTurnDelta(float rawPitchDeg, float rawYawDeg,
                          std::int64_t nowNs) noexcept;

    // Records live Field of View (degrees) from CameraAPI::tryGetFOV.
    void recordFov(float fovDeg) noexcept;

    // Records live camera perspective (0 = 1st person, 1 = 3rd back, 2 = 3rd front).
    void recordPerspective(int perspective) noexcept;

    // Called once per rendered frame (inside eglSwapBuffers) to compute the
    // optical motion vector (mvU, mvV) and scene-cut flag for Frame Generation.
    FrameMotionSample consumeFrameMotion(float aspectRatio,
                                         std::int64_t nowNs) noexcept;

    // Residual angle currently stored in the smoothing reservoir (for diagnostics/tests).
    Vec2 pendingResidual() const noexcept;

    [[nodiscard]] bool isTurnDeltaHooked() const noexcept {
        return mTurnHookInstalled.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool isFovHooked() const noexcept {
        return mFovHookInstalled.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool isPerspectiveHooked() const noexcept {
        return mPerspectiveHookInstalled.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float currentFovDeg() const noexcept {
        return mCurrentFovDeg.load(std::memory_order_relaxed);
    }
    [[nodiscard]] int currentPerspective() const noexcept {
        return mCurrentPerspective.load(std::memory_order_relaxed);
    }

    // Native detour entry points (also callable from host simulation tests)
    static void applyTurnDeltaDetour(void *self, Vec2 *turnDelta) noexcept;
    static std::uint64_t tryGetFovDetour(void *self) noexcept;
    static int getCameraPerspectiveDetour(void *self) noexcept;

    using ApplyTurnDeltaFn = void (*)(void *, Vec2 *);
    using TryGetFovFn = std::uint64_t (*)(void *);
    using GetCameraPerspectiveFn = int (*)(void *);

    void setOriginalCallbacksForTest(
        ApplyTurnDeltaFn origTurn,
        TryGetFovFn origFov = nullptr,
        GetCameraPerspectiveFn origPerspective = nullptr) noexcept;

private:
    CameraSmoother() = default;

    mutable std::mutex mMutex;

    // Cached config for low-latency access on the input/render thread
    bool mModuleEnabled = true;
    FrameGenConfig mConfig{};

    // Sub-pixel residual angle reservoir (guarantees 100% angle conservation)
    double mReservoirPitch = 0.0;
    double mReservoirYaw = 0.0;

    // Second-pole output velocity state for 2nd-order (-40 dB/dec) jitter rejection
    double mPrevStepPitch = 0.0;
    double mPrevStepYaw = 0.0;

    // Critically damped spring state (velocity in deg/s)
    double mSpringVelPitch = 0.0;
    double mSpringVelYaw = 0.0;

    // Adaptive 1-Euro filter smoothed speed state (deg/s)
    double mSmoothedSpeedDegPerSec = 0.0;

    // Hysteresis micro-deadzone accumulators
    float mDeadzoneAccPitch = 0.0f;
    float mDeadzoneAccYaw = 0.0f;

    // Accumulated smoothed camera turn during the current render frame
    double mFrameAccumPitchDeg = 0.0;
    double mFrameAccumYawDeg = 0.0;
    float mPrevMvU = 0.0f;
    float mPrevMvV = 0.0f;
    bool mPerspectiveCutPending = false;

    std::int64_t mLastTurnTimeNs = 0;
    std::int64_t mLastFrameConsumeNs = 0;

    std::atomic<float> mCurrentFovDeg{70.0f};
    std::atomic<int> mCurrentPerspective{0};

    std::atomic<bool> mTurnHookInstalled{false};
    std::atomic<bool> mFovHookInstalled{false};
    std::atomic<bool> mPerspectiveHookInstalled{false};

    pl::memory::HookHandle mTurnHook;
    pl::memory::HookHandle mFovHook;
    pl::memory::HookHandle mPerspectiveHook;

    ApplyTurnDeltaFn mOrigApplyTurnDelta = nullptr;
    TryGetFovFn mOrigTryGetFov = nullptr;
    GetCameraPerspectiveFn mOrigGetPerspective = nullptr;
};

} // namespace framegen
