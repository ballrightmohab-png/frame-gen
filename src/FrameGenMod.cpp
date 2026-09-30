#include "FrameGenMod.hpp"

#include <chrono>

#include <pl/Logger.hpp>

namespace framegen {
namespace {

constexpr int kAndroidKeycodeF8 = 138;
constexpr int kAndroidKeycodeF9 = 139;
constexpr std::int64_t kHudUpdateIntervalNs = 200'000'000LL;     // 5 Hz
constexpr std::int64_t kSchemaRefreshIntervalNs = 900'000'000LL; // ~1.1 Hz

std::int64_t steadyNowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

pl::log::Logger &modLogger() {
    static pl::log::Logger &logger = pl::log::Logger::getOrCreate(kModName);
    return logger;
}

} // namespace

FrameGenMod &FrameGenMod::instance() noexcept {
    static FrameGenMod mod;
    return mod;
}

bool FrameGenMod::load(pl::mod::ModContext &context) {
    JavaVM *vm = context.javaVm();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (vm != nullptr) {
            mJavaVm = vm;
        }
        if (!context.id().empty()) {
            mModId = context.id();
        } else {
            mModId = kModId;
        }

        if (!context.modRootPath().empty()) {
            mDataDir = context.configDir();
        } else {
            mDataDir = std::filesystem::temp_directory_path() / kModId;
        }

        const auto cfgPath = mDataDir / "config.json";
        if (!loadConfigFromFile(cfgPath, mConfig)) {
            mConfig = FrameGenConfig{};
            mConfig.normalize();
            saveConfigToFile(cfgPath, mConfig);
        }
    }

    const FrameGenConfig initialCfg = configSnapshot();
    CameraSmoother::instance().updateConfig(initialCfg, true);
    GlFrameInterpolator::instance().updateConfig(initialCfg);
    GlFrameInterpolator::instance().setModuleEnabled(true);
    HudRenderer::optimizeAndroidHudAndOverlays(vm, initialCfg, true);

    pl::input::registerKeyCallback(&FrameGenMod::onKeyEvent);

    modLogger().info("{} v{} loaded (preset: {}, camera: {})", kModName,
                     kModVersion, presetName(initialCfg.preset),
                     cameraSmoothingModeName(initialCfg.cameraSmoothingMode));
    return true;
}

bool FrameGenMod::enable(pl::mod::ModContext &context) {
    JavaVM *vm = context.javaVm();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (vm != nullptr) {
            mJavaVm = vm;
        } else {
            vm = mJavaVm;
        }
        if (!context.id().empty()) {
            mModId = context.id();
        }
    }

    mModuleEnabled.store(true, std::memory_order_relaxed);

    const bool eglOk = GlFrameInterpolator::instance().installHook();
    const bool camOk = CameraSmoother::instance().installHooks();

    GlFrameInterpolator::instance().setPostFrameCallback(
        [this](const RuntimeTelemetrySnapshot &snap) {
            this->onFramePresented(snap);
        });

    const FrameGenConfig cfg = configSnapshot();
    CameraSmoother::instance().updateConfig(cfg, true);
    GlFrameInterpolator::instance().updateConfig(cfg);
    GlFrameInterpolator::instance().setModuleEnabled(true);
    HudRenderer::optimizeAndroidHudAndOverlays(vm, cfg, true);

    std::string ownerModId;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        ownerModId = mModId;
    }

    pl::modmenu::ModuleBuilder builder(kModuleId, kModName);
    builder.modId(ownerModId)
        .description("Frame Generation (2x/3x/4x), Camera Smoothing & HUD "
                     "Optimizer.")
        .defaultEnabled(true)
        .hideInHudEditor(true)
        .onToggle(&FrameGenMod::onModuleToggle)
        .onConfigChanged(&FrameGenMod::onConfigChanged);

    appendV1ModuleConfigs(builder, cfg);

    const bool registered = builder.registerModule();
    if (!registered) {
        modLogger().warn("ModMenu V1 registration returned false; continuing");
    }

    RuntimeTelemetrySnapshot telemetry =
        GlFrameInterpolator::instance().telemetrySnapshot();
    telemetry.moduleEnabled = true;
    telemetry.eglHooked = eglOk;
    telemetry.turnDeltaHooked = CameraSmoother::instance().isTurnDeltaHooked();
    telemetry.fovHooked = CameraSmoother::instance().isFovHooked();
    telemetry.perspectiveHooked =
        CameraSmoother::instance().isPerspectiveHooked();

    const std::string schemaJson = buildV2ConfigSchemaJson(cfg, telemetry);
    HudRenderer::trySetConfigSchemaJson(kModuleId, schemaJson);

    HudRenderer::publishHud(cfg, telemetry);

    modLogger().info("{} enabled (EGL hook: {}, Camera hooks: {})", kModName,
                     eglOk ? "active" : "standby",
                     camOk ? "active" : "optical-fallback");
    return true;
}

bool FrameGenMod::disable(pl::mod::ModContext & /*context*/) {
    mModuleEnabled.store(false, std::memory_order_relaxed);
    GlFrameInterpolator::instance().setModuleEnabled(false);
    GlFrameInterpolator::instance().setPostFrameCallback(nullptr);
    CameraSmoother::instance().updateConfig(configSnapshot(), false);

    HudRenderer::clearHud();
    HudRenderer::tryClearConfigSchemaJson(kModuleId);
    pl::modmenu::unregisterModule(kModuleId);

    CameraSmoother::instance().uninstallHooks();
    GlFrameInterpolator::instance().uninstallHook();

    modLogger().info("{} disabled", kModName);
    return true;
}

bool FrameGenMod::unload(pl::mod::ModContext &context) {
    disable(context);
    saveConfigToFile(configFilePath(), configSnapshot());
    modLogger().info("{} unloaded", kModName);
    return true;
}

FrameGenConfig FrameGenMod::configSnapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mMutex);
    return mConfig;
}

std::filesystem::path FrameGenMod::configFilePath() const {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mDataDir.empty()) {
        return {};
    }
    return mDataDir / "config.json";
}

void FrameGenMod::onModuleToggle(std::string_view moduleId, bool enabled) {
    if (moduleId != kModuleId) {
        return;
    }
    instance().handleToggle(enabled);
}

void FrameGenMod::onConfigChanged(std::string_view moduleId,
                                  std::string_view key,
                                  std::string_view value) {
    if (moduleId != kModuleId) {
        return;
    }
    instance().handleConfigChange(key, value);
}

bool FrameGenMod::onKeyEvent(const pl::input::KeyEvent &event) {
    if (!event.isKeyDown) {
        return false;
    }
    if (event.keyCode == kAndroidKeycodeF8) {
        auto &mod = instance();
        mod.handleToggle(!mod.isEnabled());
        return true;
    }
    if (event.keyCode == kAndroidKeycodeF9) {
        instance().cyclePresetInGame();
        return true;
    }
    return false;
}

void FrameGenMod::handleToggle(bool enabled) {
    mModuleEnabled.store(enabled, std::memory_order_relaxed);
    const FrameGenConfig cfg = configSnapshot();

    CameraSmoother::instance().updateConfig(cfg, enabled);
    CameraSmoother::instance().reset();
    GlFrameInterpolator::instance().setModuleEnabled(enabled);
    GlFrameInterpolator::instance().requestTemporalReset();

    RuntimeTelemetrySnapshot telemetry =
        GlFrameInterpolator::instance().telemetrySnapshot();
    telemetry.moduleEnabled = enabled;

    if (enabled) {
        HudRenderer::publishHud(cfg, telemetry);
    } else {
        HudRenderer::clearHud();
    }

    HudRenderer::trySetConfigSchemaJson(
        kModuleId, buildV2ConfigSchemaJson(cfg, telemetry));
    modLogger().info("{} {}", kModName, enabled ? "resumed" : "paused");
}

void FrameGenMod::handleConfigChange(std::string_view key,
                                     std::string_view value) {
    ConfigApplyResult res = ConfigApplyResult::Unchanged;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        res = applyConfigKeyValue(mConfig, key, value);
    }

    if (res == ConfigApplyResult::Unchanged) {
        return;
    }

    if (res == ConfigApplyResult::TriggeredResetTemporal ||
        res == ConfigApplyResult::TriggeredResetDefaults ||
        res == ConfigApplyResult::TriggeredCyclePreset) {
        CameraSmoother::instance().reset();
        GlFrameInterpolator::instance().requestTemporalReset();
    }

    const bool isPositionDrag =
        (key == keys::kHudPosX || key == keys::kHudPosY || key == "hudPosX" ||
         key == "hudPosY");
    syncSubsystemsAndPersist(!isPositionDrag);
}

void FrameGenMod::cyclePresetInGame() {
    {
        std::lock_guard<std::mutex> lock(mMutex);
        const int nextIdx = (static_cast<int>(mConfig.preset) + 1) % 4;
        mConfig.applyPreset(static_cast<QualityPreset>(nextIdx));
    }
    CameraSmoother::instance().reset();
    GlFrameInterpolator::instance().requestTemporalReset();
    syncSubsystemsAndPersist(true);
}

void FrameGenMod::syncSubsystemsAndPersist(bool refreshSchema) {
    FrameGenConfig cfg;
    JavaVM *vm = nullptr;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        cfg = mConfig;
        vm = mJavaVm;
    }
    const bool enabled = mModuleEnabled.load(std::memory_order_relaxed);

    CameraSmoother::instance().updateConfig(cfg, enabled);
    GlFrameInterpolator::instance().updateConfig(cfg);
    HudRenderer::optimizeAndroidHudAndOverlays(vm, cfg, enabled);

    RuntimeTelemetrySnapshot telemetry =
        GlFrameInterpolator::instance().telemetrySnapshot();
    telemetry.moduleEnabled = enabled;

    if (enabled && cfg.showHud) {
        HudRenderer::publishHud(cfg, telemetry);
    } else {
        HudRenderer::clearHud();
    }

    saveConfigToFile(configFilePath(), cfg);

    if (refreshSchema) {
        HudRenderer::trySetConfigSchemaJson(
            kModuleId, buildV2ConfigSchemaJson(cfg, telemetry));
    }
}

void FrameGenMod::onFramePresented(const RuntimeTelemetrySnapshot &telemetry) {
    const std::int64_t now = steadyNowNs();
    const FrameGenConfig cfg = configSnapshot();

    const std::int64_t lastHud =
        mLastHudPublishNs.load(std::memory_order_relaxed);
    if (lastHud == 0 || (now - lastHud) >= kHudUpdateIntervalNs) {
        mLastHudPublishNs.store(now, std::memory_order_relaxed);
        if (telemetry.moduleEnabled && cfg.showHud) {
            HudRenderer::publishHud(cfg, telemetry);
        }
    }

    const std::int64_t lastSchema =
        mLastSchemaRefreshNs.load(std::memory_order_relaxed);
    if (lastSchema == 0 || (now - lastSchema) >= kSchemaRefreshIntervalNs) {
        mLastSchemaRefreshNs.store(now, std::memory_order_relaxed);
        HudRenderer::trySetConfigSchemaJson(
            kModuleId, buildV2ConfigSchemaJson(cfg, telemetry));
    }
}

} // namespace framegen

PL_REGISTER_MOD(framegen::FrameGenMod, framegen::FrameGenMod::instance())
