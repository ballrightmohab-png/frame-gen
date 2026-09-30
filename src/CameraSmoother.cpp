#include "CameraSmoother.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string_view>

#include <pl/memory/Signature.hpp>
#include <pl/memory/Vtable.hpp>

namespace framegen {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kBaseFovDeg = 70.0f;
constexpr float kMaxNormalizedMv = 0.22f;

// Verified ARM64 signatures for LocalPlayer::applyTurnDelta(Vec2 const&)
// from LeviLaunchroid's inbuilt GyroMod + fallback patterns across 1.21 - 1.26+.
constexpr std::string_view kApplyTurnDeltaSignatures[] = {
    "? ? ? D1 ? ? ? FD ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D5 "
    "F3 03 00 AA F4 03 01 AA ? ? ? F9 ? ? ? F8 ? ? ? F9 ? ? ? F9 ? ? ? F9",
    "? ? ? D1 ? ? ? FD ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D5 "
    "F3 03 00 AA F4 03 01 AA",
    "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 56 D0 3B D5 F3 03 00 AA F4 "
    "03 02 AA ? ? ? F9 F5 03 01 AA ? ? ? F8 ? ? ? 90"
};

std::int64_t steadyNowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

CameraSmoother &CameraSmoother::instance() noexcept {
    static CameraSmoother smoother;
    return smoother;
}

bool CameraSmoother::installHooks() noexcept {
    // 1. Resolve and hook LocalPlayer::applyTurnDelta(Vec2 const&)
    if (!mTurnHookInstalled.load(std::memory_order_relaxed)) {
        std::uintptr_t turnAddr = 0;
        for (const auto &sig : kApplyTurnDeltaSignatures) {
            turnAddr = pl::memory::resolveSignature(sig, "libminecraftpe.so");
            if (turnAddr != 0) {
                break;
            }
        }
        if (turnAddr != 0) {
            mTurnHook = pl::memory::HookHandle(
                reinterpret_cast<pl::memory::FuncPtr>(turnAddr),
                reinterpret_cast<pl::memory::FuncPtr>(&applyTurnDeltaDetour),
                reinterpret_cast<pl::memory::FuncPtr *>(&mOrigApplyTurnDelta),
                pl::memory::HookPriority::Normal);
            mTurnHookInstalled.store(mTurnHook.installed(),
                                     std::memory_order_relaxed);
        }
    }

    // 2. Resolve and hook CameraAPI::tryGetFOV (vtable slot 7 on 9CameraAPI)
    if (!mFovHookInstalled.load(std::memory_order_relaxed)) {
        const std::uintptr_t fovAddr = pl::memory::resolveVtableFunction(
            "9CameraAPI", 7, "libminecraftpe.so");
        if (fovAddr != 0) {
            mFovHook = pl::memory::HookHandle(
                reinterpret_cast<pl::memory::FuncPtr>(fovAddr),
                reinterpret_cast<pl::memory::FuncPtr>(&tryGetFovDetour),
                reinterpret_cast<pl::memory::FuncPtr *>(&mOrigTryGetFov),
                pl::memory::HookPriority::Normal);
            mFovHookInstalled.store(mFovHook.installed(),
                                    std::memory_order_relaxed);
        }
    }

    // 3. Resolve and hook VanillaCameraAPI::getCameraPerspective (vtable slot 7)
    if (!mPerspectiveHookInstalled.load(std::memory_order_relaxed)) {
        const std::uintptr_t perspAddr = pl::memory::resolveVtableFunction(
            "16VanillaCameraAPI", 7, "libminecraftpe.so");
        if (perspAddr != 0) {
            mPerspectiveHook = pl::memory::HookHandle(
                reinterpret_cast<pl::memory::FuncPtr>(perspAddr),
                reinterpret_cast<pl::memory::FuncPtr>(
                    &getCameraPerspectiveDetour),
                reinterpret_cast<pl::memory::FuncPtr *>(&mOrigGetPerspective),
                pl::memory::HookPriority::Normal);
            mPerspectiveHookInstalled.store(mPerspectiveHook.installed(),
                                            std::memory_order_relaxed);
        }
    }

    return mTurnHookInstalled.load(std::memory_order_relaxed) ||
           mFovHookInstalled.load(std::memory_order_relaxed) ||
           mPerspectiveHookInstalled.load(std::memory_order_relaxed);
}

void CameraSmoother::uninstallHooks() noexcept {
    mTurnHook.reset();
    mFovHook.reset();
    mPerspectiveHook.reset();

    mTurnHookInstalled.store(false, std::memory_order_relaxed);
    mFovHookInstalled.store(false, std::memory_order_relaxed);
    mPerspectiveHookInstalled.store(false, std::memory_order_relaxed);

    mOrigApplyTurnDelta = nullptr;
    mOrigTryGetFov = nullptr;
    mOrigGetPerspective = nullptr;

    reset();
}

void CameraSmoother::reset() noexcept {
    std::lock_guard<std::mutex> lock(mMutex);
    mReservoirPitch = 0.0;
    mReservoirYaw = 0.0;
    mPrevStepPitch = 0.0;
    mPrevStepYaw = 0.0;
    mSpringVelPitch = 0.0;
    mSpringVelYaw = 0.0;
    mSmoothedSpeedDegPerSec = 0.0;
    mDeadzoneAccPitch = 0.0f;
    mDeadzoneAccYaw = 0.0f;
    mFrameAccumPitchDeg = 0.0;
    mFrameAccumYawDeg = 0.0;
    mPerspectiveCutPending = false;
    mLastTurnTimeNs = 0;
    mLastFrameConsumeNs = 0;
}

void CameraSmoother::updateConfig(const FrameGenConfig &config,
                                  bool moduleEnabled) noexcept {
    std::lock_guard<std::mutex> lock(mMutex);
    mConfig = config;
    mConfig.normalize();
    mModuleEnabled = moduleEnabled;
}

void CameraSmoother::recordFov(float fovDeg) noexcept {
    if (std::isfinite(fovDeg) && fovDeg >= 5.0f && fovDeg <= 175.0f) {
        mCurrentFovDeg.store(fovDeg, std::memory_order_relaxed);
    }
}

void CameraSmoother::recordPerspective(int perspective) noexcept {
    const int prev =
        mCurrentPerspective.exchange(perspective, std::memory_order_relaxed);
    if (prev != perspective) {
        std::lock_guard<std::mutex> lock(mMutex);
        mPerspectiveCutPending = true;
    }
}

Vec2 CameraSmoother::processTurnDelta(float rawPitchDeg, float rawYawDeg,
                                      std::int64_t nowNs) noexcept {
    std::lock_guard<std::mutex> lock(mMutex);

    if (!std::isfinite(rawPitchDeg)) {
        rawPitchDeg = 0.0f;
    }
    if (!std::isfinite(rawYawDeg)) {
        rawYawDeg = 0.0f;
    }

    // If the module or camera smoothing is disabled, flush any residual and
    // pass raw input directly while still tracking frame motion for FrameGen.
    if (!mModuleEnabled ||
        mConfig.cameraSmoothingMode == CameraSmoothingMode::Off ||
        mConfig.cameraSmoothness <= 0.001f) {
        const float outPitch =
            static_cast<float>(mReservoirPitch) + rawPitchDeg;
        const float outYaw = static_cast<float>(mReservoirYaw) + rawYawDeg;
        mReservoirPitch = 0.0;
        mReservoirYaw = 0.0;
        mSpringVelPitch = 0.0;
        mSpringVelYaw = 0.0;
        mFrameAccumPitchDeg += static_cast<double>(outPitch);
        mFrameAccumYawDeg += static_cast<double>(outYaw);
        mLastTurnTimeNs = nowNs;
        return Vec2{outPitch, outYaw};
    }

    // 1. Clamp single-frame multi-touch / digitizer spikes
    const float spikeLimit = std::clamp(mConfig.spikeFilterDeg, 10.0f, 180.0f);
    float inPitch = std::clamp(rawPitchDeg, -spikeLimit, spikeLimit);
    float inYaw = std::clamp(rawYawDeg, -spikeLimit, spikeLimit);

    // 2. Hysteresis micro-deadzone filter: suppresses direction-reversing
    // digitizer oscillation while preserving continuous slow panning.
    const float deadzone = std::clamp(mConfig.microDeadzone, 0.0f, 0.5f);
    if (deadzone > 0.0f && (inPitch != 0.0f || inYaw != 0.0f)) {
        const float mag = std::hypot(inPitch, inYaw);
        const float dot =
            inPitch * mDeadzoneAccPitch + inYaw * mDeadzoneAccYaw;
        mDeadzoneAccPitch += inPitch;
        mDeadzoneAccYaw += inYaw;

        if (mag <= deadzone && dot < 0.0f) {
            // High-frequency direction reversal inside the micro-deadzone:
            // damp the oscillation noise without accumulating drift.
            mDeadzoneAccPitch *= 0.35f;
            mDeadzoneAccYaw *= 0.35f;
            inPitch = 0.0f;
            inYaw = 0.0f;
        } else if (std::hypot(mDeadzoneAccPitch, mDeadzoneAccYaw) >
                   deadzone * 2.0f) {
            const float scale =
                deadzone / std::hypot(mDeadzoneAccPitch, mDeadzoneAccYaw);
            mDeadzoneAccPitch *= scale;
            mDeadzoneAccYaw *= scale;
        }
    }

    // 3. Accumulate into the residual reservoir (100% angle conservation)
    mReservoirPitch += static_cast<double>(inPitch);
    mReservoirYaw += static_cast<double>(inYaw);

    // 4. Compute frame delta time dt in seconds
    double dt = 1.0 / 120.0;
    if (mLastTurnTimeNs > 0 && nowNs > mLastTurnTimeNs) {
        const double elapsedSec =
            static_cast<double>(nowNs - mLastTurnTimeNs) * 1e-9;
        if (elapsedSec <= 0.120) {
            dt = std::clamp(elapsedSec, 0.0005, 0.050);
        } else {
            // Long pause between turns: reset velocity history
            mSpringVelPitch = 0.0;
            mSpringVelYaw = 0.0;
            mSmoothedSpeedDegPerSec = 0.0;
        }
    }
    mLastTurnTimeNs = nowNs;

    // 5. FOV-aware scaling factor (steadier smoothing when zoomed in)
    double fovFactor = 1.0;
    if (mConfig.fovScaling) {
        const float liveFov = std::clamp(
            mCurrentFovDeg.load(std::memory_order_relaxed), 10.0f, 150.0f);
        if (liveFov < kBaseFovDeg) {
            fovFactor = std::clamp(
                static_cast<double>(kBaseFovDeg / liveFov), 1.0, 3.0);
        }
    }

    const double s =
        std::clamp(static_cast<double>(mConfig.cameraSmoothness), 0.0, 0.95);
    const double r = std::clamp(
        static_cast<double>(mConfig.cameraResponsiveness), 0.10, 2.50);

    double outPitch = 0.0;
    double outYaw = 0.0;

    switch (mConfig.cameraSmoothingMode) {
    case CameraSmoothingMode::Off: {
        outPitch = mReservoirPitch;
        outYaw = mReservoirYaw;
        mReservoirPitch = 0.0;
        mReservoirYaw = 0.0;
        break;
    }

    case CameraSmoothingMode::Exponential: {
        const double tau =
            (0.004 + 0.065 * std::pow(s, 1.35)) / (r / fovFactor);
        const double alpha1 =
            std::clamp(1.0 - std::exp(-dt / std::max(0.001, tau)), 0.05, 1.0);
        const double alpha2 =
            std::clamp(1.0 - std::exp(-dt / std::max(0.001, tau * 0.65)), 0.10, 1.0);

        const double targetPitch = mReservoirPitch * alpha1;
        const double targetYaw = mReservoirYaw * alpha1;

        outPitch = mPrevStepPitch + alpha2 * (targetPitch - mPrevStepPitch);
        outYaw = mPrevStepYaw + alpha2 * (targetYaw - mPrevStepYaw);

        mReservoirPitch -= outPitch;
        mReservoirYaw -= outYaw;
        mPrevStepPitch = outPitch;
        mPrevStepYaw = outYaw;
        break;
    }

    case CameraSmoothingMode::CriticallyDampedSpring: {
        const double omega =
            ((16.0 + 115.0 * std::pow(1.0 - s, 1.35)) * r) / fovFactor;
        const double expTerm = std::exp(-omega * dt);

        auto stepSpring = [&](double &res, double &vel, double &prevStep) -> double {
            if (std::abs(res) < 1e-7 && std::abs(prevStep) < 1e-7) {
                vel = 0.0;
                prevStep = 0.0;
                const double release = res;
                res = 0.0;
                return release;
            }
            const double nextRes = (res + (omega * res - vel) * dt) * expTerm;
            const double nextVel =
                (vel + (omega * omega * res - omega * vel) * dt) * expTerm;

            double clampedNextRes = nextRes;
            if ((res > 0.0 && clampedNextRes < 0.0) ||
                (res < 0.0 && clampedNextRes > 0.0)) {
                clampedNextRes = 0.0;
            } else if (std::abs(clampedNextRes) > std::abs(res)) {
                clampedNextRes = res * expTerm;
            }

            const double rawRelease = res - clampedNextRes;
            const double smoothAlpha =
                std::clamp(1.0 - expTerm, 0.12, 1.0);
            const double released =
                prevStep + smoothAlpha * (rawRelease - prevStep);
            res -= released;
            vel = nextVel;
            prevStep = released;
            return released;
        };

        outPitch = stepSpring(mReservoirPitch, mSpringVelPitch, mPrevStepPitch);
        outYaw = stepSpring(mReservoirYaw, mSpringVelYaw, mPrevStepYaw);
        break;
    }

    case CameraSmoothingMode::AdaptiveSmart: {
        // 1-Euro inspired 2-pole velocity-adaptive filter:
        // Low-pass filter the derivative signal (tau_d = 0.045s) so alternating
        // touch noise cancels out before computing the adaptive cutoff frequency.
        const double instNetSpeedDegPerSec =
            std::hypot(inPitch, inYaw) / std::max(dt, 0.001);
        const double derivAlpha =
            std::clamp(1.0 - std::exp(-dt / 0.045), 0.05, 1.0);
        mSmoothedSpeedDegPerSec +=
            derivAlpha * (instNetSpeedDegPerSec - mSmoothedSpeedDegPerSec);

        // Soft-knee velocity boost: keep cutoff low for precision aiming /
        // normal panning (< 220 deg/s), ramp up rapidly on fast combat flicks.
        const double fastFlickSpeed =
            std::max(0.0, mSmoothedSpeedDegPerSec - 220.0);
        const double minCutoffHz =
            (1.8 + 24.0 * std::pow(1.0 - s, 1.6)) / fovFactor;
        const double beta = 0.028 * std::pow(r, 1.45);
        const double cutoffHz = minCutoffHz + beta * fastFlickSpeed;
        const double tau = 1.0 / (2.0 * kPi * std::max(0.5, cutoffHz));

        const double alpha1 =
            std::clamp(1.0 - std::exp(-dt / tau), 0.05, 1.0);
        const double alpha2 =
            std::clamp(1.0 - std::exp(-dt / (tau * 0.60)), 0.10, 1.0);

        const double targetPitch = mReservoirPitch * alpha1;
        const double targetYaw = mReservoirYaw * alpha1;

        outPitch = mPrevStepPitch + alpha2 * (targetPitch - mPrevStepPitch);
        outYaw = mPrevStepYaw + alpha2 * (targetYaw - mPrevStepYaw);

        mReservoirPitch -= outPitch;
        mReservoirYaw -= outYaw;
        mPrevStepPitch = outPitch;
        mPrevStepYaw = outYaw;
        break;
    }
    }

    // Snap sub-microradian float dust to zero so the reservoir settles cleanly
    if (std::abs(mReservoirPitch) < 1e-6 && std::abs(mReservoirYaw) < 1e-6 &&
        std::abs(mPrevStepPitch) < 1e-6 && std::abs(mPrevStepYaw) < 1e-6) {
        outPitch += mReservoirPitch;
        outYaw += mReservoirYaw;
        mReservoirPitch = 0.0;
        mReservoirYaw = 0.0;
        mPrevStepPitch = 0.0;
        mPrevStepYaw = 0.0;
        mSpringVelPitch = 0.0;
        mSpringVelYaw = 0.0;
    }

    mFrameAccumPitchDeg += outPitch;
    mFrameAccumYawDeg += outYaw;

    return Vec2{static_cast<float>(outPitch), static_cast<float>(outYaw)};
}

FrameMotionSample CameraSmoother::consumeFrameMotion(
    float aspectRatio, std::int64_t nowNs) noexcept {
    std::lock_guard<std::mutex> lock(mMutex);

    FrameMotionSample sample{};
    sample.currentFovDeg = std::clamp(
        mCurrentFovDeg.load(std::memory_order_relaxed), 10.0f, 150.0f);
    sample.cameraPerspective =
        mCurrentPerspective.load(std::memory_order_relaxed);

    sample.deltaPitchDeg = static_cast<float>(mFrameAccumPitchDeg);
    sample.deltaYawDeg = static_cast<float>(mFrameAccumYawDeg);
    mFrameAccumPitchDeg = 0.0;
    mFrameAccumYawDeg = 0.0;

    sample.angularSpeedDegPerFrame =
        std::hypot(sample.deltaPitchDeg, sample.deltaYawDeg);

    // Check for scene cuts: perspective switch or angular velocity above threshold
    const bool perspectiveCut = mPerspectiveCutPending;
    mPerspectiveCutPending = false;
    const bool velocityCut =
        sample.angularSpeedDegPerFrame >= mConfig.sceneCutThreshold;
    sample.sceneCut = perspectiveCut || velocityCut;

    // Convert camera rotation (deltaYaw, deltaPitch) into normalized screen-space
    // UV motion vector (mvU, mvV).
    // When stationary (deltaYaw == 0, deltaPitch == 0), mvU == 0 and mvV == 0!
    const float safeAspect =
        (std::isfinite(aspectRatio) && aspectRatio > 0.1f) ? aspectRatio
                                                           : (16.0f / 9.0f);
    const float fovVertDeg = std::max(15.0f, sample.currentFovDeg);
    const float fovHorizDeg = std::max(15.0f, fovVertDeg * safeAspect);

    const float rawU =
        -(sample.deltaYawDeg / fovHorizDeg) * mConfig.motionScale;
    const float rawV =
        +(sample.deltaPitchDeg / fovVertDeg) * mConfig.motionScale;

    sample.mvU = std::clamp(
        std::isfinite(rawU) ? rawU : 0.0f, -kMaxNormalizedMv, kMaxNormalizedMv);
    sample.mvV = std::clamp(
        std::isfinite(rawV) ? rawV : 0.0f, -kMaxNormalizedMv, kMaxNormalizedMv);

    mLastFrameConsumeNs = nowNs;
    return sample;
}

Vec2 CameraSmoother::pendingResidual() const noexcept {
    std::lock_guard<std::mutex> lock(mMutex);
    return Vec2{static_cast<float>(mReservoirPitch),
                static_cast<float>(mReservoirYaw)};
}

void CameraSmoother::setOriginalCallbacksForTest(
    ApplyTurnDeltaFn origTurn, TryGetFovFn origFov,
    GetCameraPerspectiveFn origPerspective) noexcept {
    mOrigApplyTurnDelta = origTurn;
    mOrigTryGetFov = origFov;
    mOrigGetPerspective = origPerspective;
}

void CameraSmoother::applyTurnDeltaDetour(void *self,
                                          Vec2 *turnDelta) noexcept {
    auto &smoother = instance();
    if (turnDelta != nullptr) {
        const Vec2 smoothed = smoother.processTurnDelta(
            turnDelta->pitch, turnDelta->yaw, steadyNowNs());
        turnDelta->pitch = smoothed.pitch;
        turnDelta->yaw = smoothed.yaw;
    }
    if (smoother.mOrigApplyTurnDelta != nullptr) {
        smoother.mOrigApplyTurnDelta(self, turnDelta);
    }
}

std::uint64_t CameraSmoother::tryGetFovDetour(void *self) noexcept {
    auto &smoother = instance();
    if (smoother.mOrigTryGetFov == nullptr) {
        return 0;
    }
    const std::uint64_t packed = smoother.mOrigTryGetFov(self);
    // std::optional<float> in ARM64 AAPCS64: low 32 bits = float value,
    // byte 4 (bit 32) = bool has_value.
    const bool hasValue = ((packed >> 32) & 0xFFU) != 0U;
    if (hasValue) {
        const std::uint32_t bits = static_cast<std::uint32_t>(packed & 0xFFFFFFFFULL);
        float fov = 0.0f;
        std::memcpy(&fov, &bits, sizeof(float));
        smoother.recordFov(fov);
    }
    return packed;
}

int CameraSmoother::getCameraPerspectiveDetour(void *self) noexcept {
    auto &smoother = instance();
    if (smoother.mOrigGetPerspective == nullptr) {
        return 0;
    }
    const int perspective = smoother.mOrigGetPerspective(self);
    smoother.recordPerspective(perspective);
    return perspective;
}

} // namespace framegen
