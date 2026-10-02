#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace framegen {

// Kind of a single presentation slot inside one REAL -> GENERATED -> REAL cadence cycle.
enum class PresentSlotKind : int {
    RealFrame = 0,      // A real game-rendered frame (always slot 0 of a cycle)
    GeneratedFrame = 1  // A frame-generation synthesized frame (slots 1..multiplier-1)
};

// Resolved presentation cadence.
//
// The user-facing FPS cap ALWAYS applies to the final presented FPS (real +
// generated frames). Frame generation never bypasses the limiter:
//
//   FG OFF + 90 cap                      -> 90 presented FPS (90 real)
//   FG ON  + 90 cap, 2x                  -> 90 presented FPS (45 real + 45 generated)
//   FG ON  + 120 cap, 2x                 -> 120 presented FPS (60 real + 60 generated)
//
// The real render cadence is throttled to presentedTargetFps / multiplier and the
// generated frames fill the intervals between real frames on a single uniform,
// drift-free presentation grid.
struct FrameCadence {
    bool capActive = false;            // True when a presented-FPS cap is configured
    bool frameGenEnabled = false;
    bool extraPresentedFrames = false; // Submit generated frames as their own EGL frames
    int presentedTargetFps = 0;        // 0 = uncapped
    int realTargetFps = 0;             // presentedTargetFps / multiplier (0 = uncapped)
    int multiplier = 1;                // 1 = frame generation disabled
    std::int64_t presentedIntervalNs = 0;
    std::int64_t realIntervalNs = 0;
    int generatedPerCycle = 0;         // multiplier - 1 when FG is enabled

    [[nodiscard]] int effectiveMultiplier() const noexcept {
        return frameGenEnabled ? (multiplier < 1 ? 1 : multiplier) : 1;
    }
};

struct FramePacingDecision {
    std::int64_t wakeNs = 0;           // Monotonic time the caller may proceed at
    std::int64_t targetPresentNs = 0;  // Presentation deadline for this slot
    bool pacingActive = false;         // False when the cap is off (never touches timers)
    bool throttleWait = false;         // We actually waited for a deadline
    bool missedDeadline = false;       // The slot deadline had already passed
    bool cycleDegraded = false;        // Skip further generated frames in this cycle
    int globalSlotIndex = -1;          // Index on the uniform presentation grid
    int generatedFramesAllowed = 0;    // Generated frames permitted in this cycle
    float deadlineErrorMs = 0.0f;      // now - target (positive = late)
};

struct FramePacingMetrics {
    // Presentation FPS accounting (what the FPS counter reports)
    float realFps = 0.0f;            // Real game frames per second (render cadence)
    float generatedFps = 0.0f;       // Generated frames per second (presented)
    float presentedFps = 0.0f;       // Total presented FPS = real + generated
    float effectiveFps = 0.0f;       // Alias of presentedFps (backward compatibility)

    float onePercentLowFps = 0.0f;
    float frameTimeMs = 0.0f;
    float jitterMs = 0.0f;
    float presentedIntervalMs = 0.0f;
    float deadlineErrorMs = 0.0f;
    std::int64_t lastFrameIntervalNs = 0;
    std::int64_t emaFrameIntervalNs = 16'666'667LL;

    std::uint64_t totalRealFrames = 0;
    std::uint64_t totalSynthesizedFrames = 0;
    std::uint64_t totalPresentedFrames = 0;
    std::uint64_t missedPresentations = 0;

    int presentedTargetFps = 0;
    int realTargetFps = 0;
    int activeMultiplier = 1;
    int generatedPerCycle = 0;

    // Cap integrity monitor: proof that the configured cap is actually held.
    // capHoldPercent = share of the recent presentation windows whose measured
    // presented rate stayed at or below the configured cap.
    float capHoldPercent = 100.0f;
    std::uint64_t capExceedEvents = 0;

    bool capActive = false;
    bool pacingDegraded = false;
    bool extraPresentedFramesActive = false;
    // True when the generated sub-frame is embedded inside the real frame
    // (alpha-composited single-swap fallback). The presented frame count then
    // equals the real frame count and must never be multiplied.
    bool embeddedSynthesis = false;
    bool discontinuityDetected = false;
    bool autoTuneReduced = false;
};

class FramePacer {
public:
    static constexpr std::size_t kHistoryCapacity = 120;
    static constexpr std::int64_t kDiscontinuityThresholdNs = 120'000'000LL; // 120 ms
    static constexpr int kMinCapFps = 30;
    static constexpr int kMaxCapFps = 240;
    static constexpr std::int64_t kFpsWindowNs = 500'000'000LL; // 500 ms
    static constexpr std::int64_t kMaxPacingWaitNs = 100'000'000LL; // Safety bound

    FramePacer() = default;

    // Resolves the effective presented/real cadence from user configuration.
    // `frameGenOutputFps` is the dedicated "Frame Generation Output FPS" setting
    // (0 = Unlimited, i.e. follow the regular FPS cap). When both caps are set,
    // the TIGHTEST one wins so frame generation can never bypass the limiter.
    [[nodiscard]] static FrameCadence resolveCadence(
        int targetFps, int frameGenOutputFps, bool frameGenEnabled,
        int multiplier, bool extraPresentedFrames) noexcept;

    void reset() noexcept;

    // Waits (monotonic, drift-free, no queue accumulation) until the deadline of
    // the requested presentation slot. This is the ONLY place the module sleeps;
    // keyboard/mouse input hooks and the camera smoother never call into it, so
    // the cap never adds input latency.
    [[nodiscard]] FramePacingDecision paceSlot(const FrameCadence &cadence,
                                               PresentSlotKind kind,
                                               int indexInCycle,
                                               std::int64_t nowNs) noexcept;

    // Registers a frame that was actually presented at `tsNs` (after a successful swap).
    void recordPresented(std::int64_t tsNs, PresentSlotKind kind) noexcept;

    // Completes one REAL -> GENERATED... cycle and refreshes the metrics snapshot.
    // `embeddedSynthesis` must be true when the generated sub-frame was embedded
    // inside the real frame (alpha-composited fallback): the presented frame
    // count then equals the real frame count.
    [[nodiscard]] FramePacingMetrics endCycle(const FrameCadence &cadence,
                                              std::int64_t cycleEndNs,
                                              int realPresented,
                                              int generatedPresented,
                                              bool autoTuneEnabled,
                                              bool embeddedSynthesis = false) noexcept;

    // Legacy compatibility API (host tests / older call sites).
    [[nodiscard]] FramePacingMetrics recordFrame(std::int64_t nowNs,
                                                 int synthesizedFramesThisSwap,
                                                 int effectiveMultiplier,
                                                 bool autoTuneEnabled) noexcept;

    // Legacy compatibility wrapper: paces a single presentation slot at `targetFps`.
    std::int64_t paceForTargetFps(int targetFps, std::int64_t nowNs) noexcept;

    [[nodiscard]] FramePacingMetrics metricsSnapshot() const noexcept;

private:
    [[nodiscard]] FramePacingMetrics finishCycleLocked(std::int64_t nowNs,
                                                       int realPresented,
                                                       int generatedPresented,
                                                       int effectiveMultiplier,
                                                       bool autoTuneEnabled,
                                                       const FrameCadence *cadence) noexcept;

    mutable std::mutex mMutex;

    // Real-frame interval history (1% lows, jitter, auto-tune)
    std::array<std::int64_t, kHistoryCapacity> mIntervalHistoryNs{};
    std::size_t mHistoryWriteIdx = 0;
    std::size_t mHistoryCount = 0;

    std::int64_t mLastFrameTimeNs = 0;
    std::int64_t mPrevIntervalNs = 0;
    double mEmaIntervalNs = 16'666'667.0;
    double mEmaJitterNs = 0.0;

    // Uniform presentation grid (single source of truth for the FPS cap)
    std::int64_t mNextDeadlineNs = 0;
    std::int64_t mLastPresentedNs = 0;
    std::int64_t mLastSlotDeadlineNs = 0;
    std::int64_t mLastIntervalNs = 0;
    int mGlobalSlotIndex = 0;
    float mLastDeadlineErrorMs = 0.0f;

    // Measured presentation counters over a sliding window
    std::int64_t mFpsWindowStartNs = 0;
    int mWindowRealFrames = 0;
    int mWindowGeneratedFrames = 0;
    float mMeasuredRealFps = 0.0f;
    float mMeasuredGeneratedFps = 0.0f;
    float mMeasuredPresentedFps = 0.0f;

    std::uint64_t mTotalRealFrames = 0;
    std::uint64_t mTotalSynthesizedFrames = 0;
    std::uint64_t mTotalPresentedFrames = 0;
    std::uint64_t mMissedPresentations = 0;

    // Cap integrity monitor
    float mCapHoldPercent = 100.0f;
    std::uint64_t mCapExceedEvents = 0;

    // Graceful degradation when the real renderer cannot keep up
    int mConsecutiveLateCycles = 0;
    int mConsecutiveOnTimeCycles = 0;
    bool mExtraFramesThrottled = false;

    int mOverloadStreak = 0;
    int mStableStreak = 0;
    bool mAutoTuneReduced = false;

    FramePacingMetrics mLatestMetrics{};
};

} // namespace framegen
