#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace framegen {

struct FramePacingMetrics {
    float realFps = 0.0f;
    float effectiveFps = 0.0f;
    float onePercentLowFps = 0.0f;
    float frameTimeMs = 0.0f;
    float jitterMs = 0.0f;
    std::int64_t lastFrameIntervalNs = 0;
    std::int64_t emaFrameIntervalNs = 16'666'667LL;
    std::uint64_t totalRealFrames = 0;
    std::uint64_t totalSynthesizedFrames = 0;
    bool discontinuityDetected = false;
    bool autoTuneReduced = false;
};

class FramePacer {
public:
    static constexpr std::size_t kHistoryCapacity = 120;
    static constexpr std::int64_t kDiscontinuityThresholdNs = 120'000'000LL; // 120 ms

    FramePacer() = default;

    void reset() noexcept;

    // Records a completed real game frame and any extra synthesized frames
    // generated during this swap cycle. Returns updated metrics.
    FramePacingMetrics recordFrame(std::int64_t nowNs,
                                   int synthesizedFramesThisSwap,
                                   int effectiveMultiplier,
                                   bool autoTuneEnabled) noexcept;

    // Optional frame cap pacer. If targetFps >= 30, sleeps/yields up to the
    // next target frame deadline (bounded to avoid render-thread stalls).
    std::int64_t paceForTargetFps(int targetFps, std::int64_t nowNs) noexcept;

    [[nodiscard]] FramePacingMetrics metricsSnapshot() const noexcept;

private:
    mutable std::mutex mMutex;

    std::array<std::int64_t, kHistoryCapacity> mIntervalHistoryNs{};
    std::size_t mHistoryWriteIdx = 0;
    std::size_t mHistoryCount = 0;

    std::int64_t mLastFrameTimeNs = 0;
    std::int64_t mPrevIntervalNs = 0;
    std::int64_t mNextTargetDeadlineNs = 0;
    double mEmaIntervalNs = 16'666'667.0;
    double mEmaJitterNs = 0.0;

    std::uint64_t mTotalRealFrames = 0;
    std::uint64_t mTotalSynthesizedFrames = 0;

    int mOverloadStreak = 0;
    int mStableStreak = 0;
    bool mAutoTuneReduced = false;

    FramePacingMetrics mLatestMetrics{};
};

} // namespace framegen
