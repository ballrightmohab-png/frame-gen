#include "FramePacer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace framegen {

void FramePacer::reset() noexcept {
    std::lock_guard<std::mutex> lock(mMutex);
    mIntervalHistoryNs.fill(0);
    mHistoryWriteIdx = 0;
    mHistoryCount = 0;
    mLastFrameTimeNs = 0;
    mPrevIntervalNs = 0;
    mNextTargetDeadlineNs = 0;
    mEmaIntervalNs = 16'666'667.0;
    mEmaJitterNs = 0.0;
    mTotalRealFrames = 0;
    mTotalSynthesizedFrames = 0;
    mOverloadStreak = 0;
    mStableStreak = 0;
    mAutoTuneReduced = false;
    mLatestMetrics = FramePacingMetrics{};
}

FramePacingMetrics FramePacer::recordFrame(std::int64_t nowNs,
                                           int synthesizedFramesThisSwap,
                                           int effectiveMultiplier,
                                           bool autoTuneEnabled) noexcept {
    std::lock_guard<std::mutex> lock(mMutex);

    ++mTotalRealFrames;
    if (synthesizedFramesThisSwap > 0) {
        mTotalSynthesizedFrames +=
            static_cast<std::uint64_t>(synthesizedFramesThisSwap);
    }

    FramePacingMetrics out{};
    out.totalRealFrames = mTotalRealFrames;
    out.totalSynthesizedFrames = mTotalSynthesizedFrames;

    if (mLastFrameTimeNs <= 0 || nowNs <= mLastFrameTimeNs) {
        mLastFrameTimeNs = nowNs;
        out.emaFrameIntervalNs = static_cast<std::int64_t>(mEmaIntervalNs);
        out.frameTimeMs = static_cast<float>(mEmaIntervalNs * 1e-6);
        mLatestMetrics = out;
        return out;
    }

    const std::int64_t dtNs = nowNs - mLastFrameTimeNs;
    mLastFrameTimeNs = nowNs;
    out.lastFrameIntervalNs = dtNs;

    if (dtNs >= kDiscontinuityThresholdNs) {
        out.discontinuityDetected = true;
        // Do not pollute EMA with menu/loading stalls
        out.emaFrameIntervalNs = static_cast<std::int64_t>(mEmaIntervalNs);
        out.frameTimeMs = static_cast<float>(mEmaIntervalNs * 1e-6);
        out.jitterMs = static_cast<float>(mEmaJitterNs * 1e-6);
        out.realFps = mLatestMetrics.realFps;
        out.effectiveFps = mLatestMetrics.effectiveFps;
        out.onePercentLowFps = mLatestMetrics.onePercentLowFps;
        out.autoTuneReduced = mAutoTuneReduced;
        mLatestMetrics = out;
        return out;
    }

    const double clampedDtNs =
        std::clamp(static_cast<double>(dtNs), 1'000'000.0, 100'000'000.0);

    if (mHistoryCount == 0 && clampedDtNs >= 4'000'000.0) {
        mEmaIntervalNs = clampedDtNs;
        mEmaJitterNs = 0.0;
    } else {
        const double prevDtNs =
            (mPrevIntervalNs > 0) ? static_cast<double>(mPrevIntervalNs)
                                  : mEmaIntervalNs;
        const double diffNs = std::abs(clampedDtNs - prevDtNs);
        mEmaJitterNs += 0.22 * (diffNs - mEmaJitterNs);
        mEmaIntervalNs += 0.12 * (clampedDtNs - mEmaIntervalNs);
    }
    mPrevIntervalNs = static_cast<std::int64_t>(clampedDtNs);

    mIntervalHistoryNs[mHistoryWriteIdx] =
        static_cast<std::int64_t>(clampedDtNs);
    mHistoryWriteIdx = (mHistoryWriteIdx + 1) % kHistoryCapacity;
    if (mHistoryCount < kHistoryCapacity) {
        ++mHistoryCount;
    }

    // Compute average real FPS and 1% low FPS from the rolling window
    double sumIntervalNs = 0.0;
    std::int64_t worstIntervalNs = 0;
    for (std::size_t i = 0; i < mHistoryCount; ++i) {
        const std::int64_t val = mIntervalHistoryNs[i];
        sumIntervalNs += static_cast<double>(val);
        if (val > worstIntervalNs) {
            worstIntervalNs = val;
        }
    }

    const double meanIntervalNs =
        (mHistoryCount > 0)
            ? (sumIntervalNs / static_cast<double>(mHistoryCount))
            : mEmaIntervalNs;

    out.realFps =
        (meanIntervalNs > 0.0)
            ? static_cast<float>(1'000'000'000.0 / meanIntervalNs)
            : 0.0f;

    const int mult = std::max(1, effectiveMultiplier);
    out.effectiveFps = out.realFps * static_cast<float>(mult);

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
        const bool overloaded =
            (out.jitterMs > 4.5f) || (clampedDtNs > 28'500'000.0);
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

std::int64_t FramePacer::paceForTargetFps(int targetFps,
                                          std::int64_t nowNs) noexcept {
    if (targetFps < 30 || targetFps > 240) {
        mNextTargetDeadlineNs = 0;
        return nowNs;
    }

    const std::int64_t targetIntervalNs =
        1'000'000'000LL / static_cast<std::int64_t>(targetFps);

    std::int64_t deadlineNs = 0;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mNextTargetDeadlineNs <= 0 ||
            nowNs > mNextTargetDeadlineNs + targetIntervalNs) {
            mNextTargetDeadlineNs = nowNs + targetIntervalNs;
            return nowNs;
        }
        deadlineNs = mNextTargetDeadlineNs;
        mNextTargetDeadlineNs += targetIntervalNs;
    }

    const std::int64_t waitNs = deadlineNs - nowNs;
    if (waitNs > 200'000LL && waitNs <= 35'000'000LL) {
        std::this_thread::sleep_for(
            std::chrono::nanoseconds(waitNs - 150'000LL));
    }

    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

FramePacingMetrics FramePacer::metricsSnapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mMutex);
    return mLatestMetrics;
}

} // namespace framegen
