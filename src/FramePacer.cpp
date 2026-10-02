#include "FramePacer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace framegen {
namespace {

std::int64_t steadyNowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// High-resolution, monotonic deadline wait.
// Coarse sleep for everything above the spin window, then a strictly bounded
// yield-spin for the final ~200 us (Android sleep_for() jitter is far larger
// than one frame at 90/120 FPS, so a pure sleep would visibly beat).
std::int64_t waitUntilDeadlineNs(std::int64_t deadlineNs) noexcept {
    constexpr std::int64_t kSpinWindowNs = 200'000LL; // 200 us
    constexpr int kMaxSpinIterations = 512;

    std::int64_t now = steadyNowNs();
    const std::int64_t coarseNs = deadlineNs - now - kSpinWindowNs;
    if (coarseNs > 0) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(coarseNs));
    }

    for (int i = 0; i < kMaxSpinIterations; ++i) {
        now = steadyNowNs();
        if (now >= deadlineNs) {
            return now;
        }
        std::this_thread::yield();
    }
    return steadyNowNs();
}

} // namespace

FrameCadence FramePacer::resolveCadence(int targetFps,
                                        int frameGenOutputFps,
                                        bool frameGenEnabled,
                                        int multiplier,
                                        bool extraPresentedFrames) noexcept {
    FrameCadence cadence{};
    cadence.frameGenEnabled = frameGenEnabled;
    cadence.multiplier =
        frameGenEnabled ? std::clamp(multiplier, 2, 4) : 1;
    cadence.generatedPerCycle = cadence.multiplier - 1;

    if (frameGenEnabled) {
        // The FPS cap is never bypassed by frame generation: when both the
        // regular cap and the FG output cap are configured, the TIGHTEST one
        // applies to the final presented FPS.
        int cap = 0;
        const int candidates[2] = {targetFps, frameGenOutputFps};
        for (const int value : candidates) {
            if (value >= kMinCapFps && value <= kMaxCapFps) {
                cap = (cap == 0) ? value : std::min(cap, value);
            }
        }
        cadence.presentedTargetFps = cap;
        cadence.realTargetFps =
            (cap > 0) ? std::max(kMinCapFps / 4, cap / cadence.multiplier) : 0;
        cadence.extraPresentedFrames = extraPresentedFrames;
    } else {
        cadence.presentedTargetFps =
            (targetFps >= kMinCapFps && targetFps <= kMaxCapFps) ? targetFps : 0;
        cadence.realTargetFps = cadence.presentedTargetFps;
        cadence.extraPresentedFrames = false;
    }

    cadence.capActive = cadence.presentedTargetFps >= kMinCapFps;
    if (cadence.capActive) {
        cadence.presentedIntervalNs =
            1'000'000'000LL / static_cast<std::int64_t>(cadence.presentedTargetFps);
        cadence.realIntervalNs =
            cadence.presentedIntervalNs * static_cast<std::int64_t>(cadence.multiplier);
    }
    return cadence;
}

void FramePacer::reset() noexcept {
    std::lock_guard<std::mutex> lock(mMutex);
    mIntervalHistoryNs.fill(0);
    mHistoryWriteIdx = 0;
    mHistoryCount = 0;
    mLastFrameTimeNs = 0;
    mPrevIntervalNs = 0;
    mEmaIntervalNs = 16'666'667.0;
    mEmaJitterNs = 0.0;

    mNextDeadlineNs = 0;
    mLastPresentedNs = 0;
    mLastSlotDeadlineNs = 0;
    mGlobalSlotIndex = 0;
    mLastDeadlineErrorMs = 0.0f;

    mFpsWindowStartNs = 0;
    mWindowRealFrames = 0;
    mWindowGeneratedFrames = 0;
    mMeasuredRealFps = 0.0f;
    mMeasuredGeneratedFps = 0.0f;
    mMeasuredPresentedFps = 0.0f;

    mTotalRealFrames = 0;
    mTotalSynthesizedFrames = 0;
    mTotalPresentedFrames = 0;
    mMissedPresentations = 0;

    mCapHoldPercent = 100.0f;
    mCapExceedEvents = 0;

    mConsecutiveLateCycles = 0;
    mConsecutiveOnTimeCycles = 0;
    mExtraFramesThrottled = false;

    mOverloadStreak = 0;
    mStableStreak = 0;
    mAutoTuneReduced = false;
    mLatestMetrics = FramePacingMetrics{};
}

FramePacingDecision FramePacer::paceSlot(const FrameCadence &cadence,
                                         PresentSlotKind kind,
                                         int indexInCycle,
                                         std::int64_t nowNs) noexcept {
    FramePacingDecision decision{};
    decision.wakeNs = nowNs;
    decision.targetPresentNs = nowNs;

    // Uncapped: the limiter must never touch the clock. Frame generation then
    // adds zero presentation latency, but the cadence still reports how many
    // generated frames belong to this cycle.
    if (!cadence.capActive || cadence.presentedIntervalNs <= 0) {
        std::lock_guard<std::mutex> lock(mMutex);
        decision.generatedFramesAllowed =
            std::max(0, cadence.generatedPerCycle);
        if (mAutoTuneReduced) {
            decision.generatedFramesAllowed =
                std::min(decision.generatedFramesAllowed, 1);
        }
        return decision;
    }

    const std::int64_t intervalNs = cadence.presentedIntervalNs;
    std::int64_t deadlineNs = 0;
    bool missed = false;
    bool degraded = false;
    int allowedGenerated = std::max(0, cadence.generatedPerCycle);
    int slotIndex = 0;

    {
        std::lock_guard<std::mutex> lock(mMutex);

        // Re-anchor after a long stall / clock anomaly (menu, loading screen)
        // and whenever the user changes the cadence (cap or multiplier), so a
        // stale grid can never add latency or a burst of catch-up frames.
        if (mNextDeadlineNs <= 0 ||
            nowNs + 1'000'000'000LL < mNextDeadlineNs ||
            intervalNs != mLastIntervalNs) {
            mNextDeadlineNs = nowNs;
        }
        mLastIntervalNs = intervalNs;

        deadlineNs = mNextDeadlineNs;

        const std::int64_t lateToleranceNs = intervalNs / 2;
        if (nowNs > deadlineNs + lateToleranceNs) {
            // The real renderer could not hit this slot: snap the grid FORWARD to
            // the next boundary instead of queueing up presentations (no backlog,
            // no burst catch-up, no accumulated input latency).
            const std::int64_t behindNs = nowNs - deadlineNs;
            const std::int64_t missedSlots =
                (behindNs + intervalNs - 1) / intervalNs;
            deadlineNs += missedSlots * intervalNs;
            if (deadlineNs < nowNs) {
                deadlineNs = nowNs;
            }
            missed = true;
            mMissedPresentations += static_cast<std::uint64_t>(
                std::max<std::int64_t>(1, missedSlots));

            if (kind == PresentSlotKind::RealFrame) {
                ++mConsecutiveLateCycles;
                mConsecutiveOnTimeCycles = 0;
                if (mConsecutiveLateCycles >= 2) {
                    mExtraFramesThrottled = true;
                }
            } else {
                // Slowed down mid-cycle: present this one, then stop generating
                // extra frames until the real cadence recovers.
                mExtraFramesThrottled = true;
                degraded = true;
                allowedGenerated = std::min(allowedGenerated, indexInCycle);
            }
        } else if (kind == PresentSlotKind::RealFrame) {
            ++mConsecutiveOnTimeCycles;
            if (mConsecutiveOnTimeCycles >= 45) {
                // ~0.5 s of stable real cadence: re-enable the full multiplier.
                mExtraFramesThrottled = false;
                mConsecutiveLateCycles = 0;
            }
        }

        if (mExtraFramesThrottled) {
            allowedGenerated = std::min(allowedGenerated, 1);
            degraded = degraded || (cadence.generatedPerCycle > 1);
        }
        if (mAutoTuneReduced) {
            allowedGenerated = std::min(allowedGenerated, 1);
        }
        if (kind == PresentSlotKind::GeneratedFrame) {
            allowedGenerated = std::min(allowedGenerated, indexInCycle);
        }

        mNextDeadlineNs = deadlineNs + intervalNs;
        slotIndex = mGlobalSlotIndex++;
        mLastSlotDeadlineNs = deadlineNs;
    }

    decision.pacingActive = true;
    decision.targetPresentNs = deadlineNs;
    decision.globalSlotIndex = slotIndex;
    decision.generatedFramesAllowed = allowedGenerated;
    decision.cycleDegraded = degraded;
    decision.missedDeadline = missed;

    // Honest low-latency: when the renderer is already late and throttled,
    // don't add 0-11 ms of extra wait to re-align to the grid. Present
    // immediately and re-anchor next to now+interval. This makes low FPS
    // (25) feel like 25, not 25+wait.
    bool skipWaitWhenLate = false;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        skipWaitWhenLate = missed && (mExtraFramesThrottled || mAutoTuneReduced ||
                                      mConsecutiveLateCycles >= 1);
        if (skipWaitWhenLate) {
            decision.targetPresentNs = nowNs;
            mNextDeadlineNs = nowNs + intervalNs;
            mLastSlotDeadlineNs = nowNs;
        }
    }

    const std::int64_t waitNs = decision.targetPresentNs - nowNs;
    if (!skipWaitWhenLate && waitNs > 0 && waitNs <= kMaxPacingWaitNs) {
        decision.wakeNs = waitUntilDeadlineNs(decision.targetPresentNs);
        decision.throttleWait = true;
    } else {
        decision.wakeNs = nowNs;
        if (skipWaitWhenLate) {
            decision.deadlineErrorMs = 0.0f;
            std::lock_guard<std::mutex> lock(mMutex);
            mLastDeadlineErrorMs = 0.0f;
            return decision;
        }
    }

    decision.deadlineErrorMs =
        static_cast<float>(decision.wakeNs - deadlineNs) * 1e-6f;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mLastDeadlineErrorMs = decision.deadlineErrorMs;
    }
    return decision;
}

void FramePacer::recordPresented(std::int64_t tsNs,
                                 PresentSlotKind kind) noexcept {
    std::lock_guard<std::mutex> lock(mMutex);

    mLastPresentedNs = tsNs;
    ++mTotalPresentedFrames;
    if (kind == PresentSlotKind::RealFrame) {
        ++mTotalRealFrames;
    } else {
        ++mTotalSynthesizedFrames;
    }

    // Sliding-window presentation counter: this is the ground truth used for
    // the reported Real / Generated / Presented FPS.
    if (mFpsWindowStartNs <= 0 || tsNs <= mFpsWindowStartNs) {
        mFpsWindowStartNs = tsNs;
        mWindowRealFrames = 0;
        mWindowGeneratedFrames = 0;
    }
    if (kind == PresentSlotKind::RealFrame) {
        ++mWindowRealFrames;
    } else {
        ++mWindowGeneratedFrames;
    }

    const std::int64_t spanNs = tsNs - mFpsWindowStartNs;
    if (spanNs >= kFpsWindowNs) {
        const double spanS = static_cast<double>(spanNs) * 1e-9;
        const int totalInWindow = mWindowRealFrames + mWindowGeneratedFrames;
        if (spanS > 1e-6 && totalInWindow > 0) {
            const float realFps =
                static_cast<float>(static_cast<double>(mWindowRealFrames) / spanS);
            const float genFps = static_cast<float>(
                static_cast<double>(mWindowGeneratedFrames) / spanS);
            // Fast re-acquisition after a stall, gentle smoothing when healthy.
            const float alpha = (spanNs > 2 * kFpsWindowNs) ? 1.0f : 0.45f;
            mMeasuredRealFps = (mMeasuredRealFps <= 0.0f)
                                   ? realFps
                                   : (1.0f - alpha) * mMeasuredRealFps + alpha * realFps;
            mMeasuredGeneratedFps =
                (mMeasuredGeneratedFps <= 0.0f)
                    ? genFps
                    : (1.0f - alpha) * mMeasuredGeneratedFps + alpha * genFps;
            mMeasuredPresentedFps = mMeasuredRealFps + mMeasuredGeneratedFps;
        }
        mFpsWindowStartNs = tsNs;
        mWindowRealFrames = 0;
        mWindowGeneratedFrames = 0;
    }
}

FramePacingMetrics FramePacer::endCycle(const FrameCadence &cadence,
                                        std::int64_t cycleEndNs,
                                        int realPresented,
                                        int generatedPresented,
                                        bool autoTuneEnabled,
                                        bool embeddedSynthesis) noexcept {
    std::lock_guard<std::mutex> lock(mMutex);
    FramePacingMetrics out = finishCycleLocked(
        cycleEndNs, realPresented, generatedPresented,
        cadence.effectiveMultiplier(), autoTuneEnabled, &cadence);
    if (embeddedSynthesis) {
        // The generated sub-frame was embedded inside the real frame (composited
        // fallback): the panel still receives exactly `realPresented` frames, so
        // the presented FPS must be reported as the real rate and can never be
        // multiplied above the configured cap.
        out.embeddedSynthesis = true;
        out.generatedFps = 0.0f;
        out.presentedFps = (mMeasuredPresentedFps > 0.5f) ? mMeasuredPresentedFps
                                                          : out.realFps;
        out.effectiveFps = out.presentedFps;
        if (out.presentedIntervalMs <= 0.0f && out.presentedFps > 0.5f) {
            out.presentedIntervalMs = 1000.0f / out.presentedFps;
        }
        mLatestMetrics = out;
    }
    return out;
}

FramePacingMetrics FramePacer::recordFrame(std::int64_t nowNs,
                                           int synthesizedFramesThisSwap,
                                           int effectiveMultiplier,
                                           bool autoTuneEnabled) noexcept {
    std::lock_guard<std::mutex> lock(mMutex);
    if (synthesizedFramesThisSwap > 0) {
        mTotalSynthesizedFrames +=
            static_cast<std::uint64_t>(synthesizedFramesThisSwap);
    }
    ++mTotalRealFrames;
    return finishCycleLocked(nowNs, 1, synthesizedFramesThisSwap,
                             effectiveMultiplier, autoTuneEnabled, nullptr);
}

std::int64_t FramePacer::paceForTargetFps(int targetFps,
                                          std::int64_t nowNs) noexcept {
    if (targetFps < kMinCapFps || targetFps > kMaxCapFps) {
        return nowNs;
    }
    const FrameCadence cadence =
        resolveCadence(targetFps, 0, false, 1, false);
    return paceSlot(cadence, PresentSlotKind::RealFrame, 0, nowNs).wakeNs;
}

FramePacingMetrics FramePacer::finishCycleLocked(std::int64_t nowNs,
                                                 int realPresented,
                                                 int generatedPresented,
                                                 int effectiveMultiplier,
                                                 bool autoTuneEnabled,
                                                 const FrameCadence *cadence) noexcept {
    FramePacingMetrics out{};
    out.totalRealFrames = mTotalRealFrames;
    out.totalSynthesizedFrames = mTotalSynthesizedFrames;
    out.totalPresentedFrames = mTotalPresentedFrames;
    out.missedPresentations = mMissedPresentations;

    const int mult = std::max(1, effectiveMultiplier);
    out.activeMultiplier = mult;
    out.generatedPerCycle = std::max(0, generatedPresented);
    out.autoTuneReduced = mAutoTuneReduced;

    if (cadence != nullptr) {
        out.capActive = cadence->capActive;
        out.presentedTargetFps = cadence->presentedTargetFps;
        out.realTargetFps = cadence->realTargetFps;
        out.extraPresentedFramesActive =
            cadence->frameGenEnabled && cadence->extraPresentedFrames;
        out.presentedIntervalMs =
            (cadence->presentedIntervalNs > 0)
                ? static_cast<float>(
                      static_cast<double>(cadence->presentedIntervalNs) * 1e-6)
                : 0.0f;
        out.pacingDegraded = mExtraFramesThrottled;
        out.deadlineErrorMs = mLastDeadlineErrorMs;
    }

    if (realPresented > 0 && mLastFrameTimeNs <= 0) {
        // First real frame: seed the time base without polluting the EMA.
        mLastFrameTimeNs = nowNs;
    } else if (realPresented > 0 && nowNs > mLastFrameTimeNs) {
        const std::int64_t dtNs = nowNs - mLastFrameTimeNs;
        mLastFrameTimeNs = nowNs;
        out.lastFrameIntervalNs = dtNs;

        if (dtNs >= kDiscontinuityThresholdNs) {
            out.discontinuityDetected = true;
            // Do not pollute the EMA with menu/loading stalls.
        } else if (dtNs > 0) {
            const double clampedDtNs = std::clamp(
                static_cast<double>(dtNs), 1'000'000.0, 100'000'000.0);

            if (mHistoryCount == 0 && clampedDtNs >= 4'000'000.0) {
                mEmaIntervalNs = clampedDtNs;
                mEmaJitterNs = 0.0;
            } else {
                const double prevDtNs = (mPrevIntervalNs > 0)
                                            ? static_cast<double>(mPrevIntervalNs)
                                            : mEmaIntervalNs;
                const double diffNs = std::abs(clampedDtNs - prevDtNs);
                mEmaJitterNs += 0.22 * (diffNs - mEmaJitterNs);

                // Outlier-resistant pacing EMA: a single isolated spike must not
                // skew the evenly spaced REAL -> GEN -> REAL presentation grid.
                const double pacedSampleNs =
                    (mHistoryCount >= 4 && mOverloadStreak == 0)
                        ? std::clamp(clampedDtNs, mEmaIntervalNs * 0.60,
                                     mEmaIntervalNs * 1.40)
                        : clampedDtNs;
                mEmaIntervalNs += 0.14 * (pacedSampleNs - mEmaIntervalNs);
            }
            mPrevIntervalNs = static_cast<std::int64_t>(clampedDtNs);

            mIntervalHistoryNs[mHistoryWriteIdx] =
                static_cast<std::int64_t>(clampedDtNs);
            mHistoryWriteIdx = (mHistoryWriteIdx + 1) % kHistoryCapacity;
            if (mHistoryCount < kHistoryCapacity) {
                ++mHistoryCount;
            }
        }
    }

    double sumIntervalNs = 0.0;
    std::int64_t worstIntervalNs = 0;
    for (std::size_t i = 0; i < mHistoryCount; ++i) {
        const std::int64_t value = mIntervalHistoryNs[i];
        sumIntervalNs += static_cast<double>(value);
        if (value > worstIntervalNs) {
            worstIntervalNs = value;
        }
    }

    const double meanIntervalNs =
        (mHistoryCount > 0) ? (sumIntervalNs / static_cast<double>(mHistoryCount))
                            : mEmaIntervalNs;

    out.realFps = (meanIntervalNs > 0.0)
                      ? static_cast<float>(1'000'000'000.0 / meanIntervalNs)
                      : 0.0f;

    // Presentation FPS accounting - measured from ACTUAL EGL presentations.
    // A presented frame is a frame the display pipeline receives, so the report
    // can never exceed the configured cap:
    //  * own presented frames (REAL -> GEN -> REAL):  real + generated
    //  * embedded synthesis (composited single swap): real only (gen embedded)
    //  * legacy recordFrame():                        realFps * multiplier
    const bool frameGenOn = (cadence != nullptr) && cadence->frameGenEnabled;
    const bool embeddedMode =
        frameGenOn && !(cadence->extraPresentedFrames);
    const float measuredPresented = mMeasuredPresentedFps;
    if (embeddedMode) {
        out.embeddedSynthesis = true;
        out.presentedFps = (measuredPresented > 0.5f) ? measuredPresented
                                                     : out.realFps;
        out.generatedFps = 0.0f;
    } else if (measuredPresented > 0.5f && mMeasuredRealFps > 0.5f) {
        out.presentedFps = measuredPresented;
        out.generatedFps = std::max(0.0f, mMeasuredGeneratedFps);
    } else {
        out.presentedFps = out.realFps * static_cast<float>(mult);
        out.generatedFps = std::max(0.0f, out.presentedFps - out.realFps);
    }
    out.effectiveFps = out.presentedFps;

    // Cap integrity monitor: verify the measured presented rate really is held at
    // or below the configured cap (12% allowance for windowing/jitter noise).
    out.capHoldPercent = mCapHoldPercent;
    out.capExceedEvents = mCapExceedEvents;
    if (cadence != nullptr && cadence->capActive &&
        cadence->presentedTargetFps > 0 && measuredPresented > 0.5f) {
        const float limit =
            static_cast<float>(cadence->presentedTargetFps) * 1.12f;
        if (measuredPresented > limit) {
            ++mCapExceedEvents;
            mCapHoldPercent = 0.75f * mCapHoldPercent;
        } else {
            mCapHoldPercent = 0.75f * mCapHoldPercent + 0.25f * 100.0f;
        }
        out.capHoldPercent = mCapHoldPercent;
        out.capExceedEvents = mCapExceedEvents;
    }
    if (out.presentedIntervalMs <= 0.0f && out.presentedFps > 0.5f) {
        out.presentedIntervalMs = 1000.0f / out.presentedFps;
    }

    out.onePercentLowFps =
        (worstIntervalNs > 0)
            ? static_cast<float>(1'000'000'000.0 /
                                 static_cast<double>(worstIntervalNs))
            : out.realFps;

    out.frameTimeMs = static_cast<float>(mEmaIntervalNs * 1e-6);
    out.jitterMs = static_cast<float>(mEmaJitterNs * 1e-6);
    out.emaFrameIntervalNs = static_cast<std::int64_t>(mEmaIntervalNs);

    // Smart Auto-Tune hysteresis:
    // If jitter exceeds 4.5 ms or frame time exceeds 28 ms (~35 FPS) for 8+ frames,
    // reduce extra workload; recover after 30 consecutive stable frames.
    if (!autoTuneEnabled) {
        mAutoTuneReduced = false;
        mOverloadStreak = 0;
        mStableStreak = 0;
    } else {
        const double latestIntervalNs =
            (out.lastFrameIntervalNs > 0)
                ? static_cast<double>(out.lastFrameIntervalNs)
                : mEmaIntervalNs;
        const bool overloaded =
            (out.jitterMs > 4.5f) || (latestIntervalNs > 28'500'000.0);
        if (overloaded) {
            ++mOverloadStreak;
            mStableStreak = 0;
            if (mOverloadStreak >= 8) {
                mAutoTuneReduced = true;
            }
        } else {
            ++mStableStreak;
            mOverloadStreak = std::max(0, mOverloadStreak - 1);
            if (mStableStreak >= 30) {
                mAutoTuneReduced = false;
            }
        }
    }

    out.autoTuneReduced = mAutoTuneReduced;
    mLatestMetrics = out;
    return out;
}

FramePacingMetrics FramePacer::metricsSnapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mMutex);
    return mLatestMetrics;
}

} // namespace framegen
