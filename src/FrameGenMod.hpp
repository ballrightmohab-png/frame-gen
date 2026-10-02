#pragma once

#include "CameraSmoother.hpp"
#include "FrameGenConfig.hpp"
#include "GlFrameInterpolator.hpp"
#include "HudRenderer.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

#include <pl/Input.hpp>
#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>

namespace framegen {

class FrameGenMod {
public:
    static FrameGenMod &instance() noexcept;

    bool load(pl::mod::ModContext &context);
    bool enable(pl::mod::ModContext &context);
    bool disable(pl::mod::ModContext &context);
    bool unload(pl::mod::ModContext &context);

    // Direct accessors for testing & runtime inspection
    [[nodiscard]] FrameGenConfig configSnapshot() const noexcept;
    [[nodiscard]] bool isEnabled() const noexcept {
        return mModuleEnabled.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::filesystem::path configFilePath() const;

    // ModMenu & input callbacks
    static void onModuleToggle(std::string_view moduleId, bool enabled);
    static void onConfigChanged(std::string_view moduleId,
                                std::string_view key,
                                std::string_view value);
    static bool onKeyEvent(const pl::input::KeyEvent &event);

private:
    FrameGenMod() = default;

    void handleToggle(bool enabled);
    void handleConfigChange(std::string_view key, std::string_view value);
    void cyclePresetInGame();
    void syncSubsystemsAndPersist(bool refreshSchema);
    void onFramePresented(const RuntimeTelemetrySnapshot &telemetry);

    mutable std::mutex mMutex;
    FrameGenConfig mConfig{};
    std::string mModId{kModId};
    std::filesystem::path mDataDir{};
    JavaVM *mJavaVm = nullptr;

    std::atomic<bool> mModuleEnabled{true};
    std::atomic<std::int64_t> mLastHudPublishNs{0};
    std::atomic<std::int64_t> mLastSchemaRefreshNs{0};
};

} // namespace framegen
