#include "FrameGenMod.hpp"

#include <chrono>

#include <pl/Logger.hpp>

namespace framegen {
namespace {

constexpr int kAndroidKeycodeF8 = 138;
constexpr int kAndroidKeycodeF9 = 139;
constexpr std::int64_t kHudUpdateIntervalNs = 200'000'000LL;     // 5 Hz
constexpr std::int64_t kSchemaRefreshIntervalNs = 900'000'000LL; // ~1.1 Hz

constexpr const char *kQuickButtonSvgIcon = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">
  <rect x="6" y="6" width="52" height="52" rx="14" fill="#121B24" stroke="#38E8B0" stroke-width="3"/>
  <path d="M20 40 L28 24 L36 34 L44 20" fill="none" stroke="#38E8B0" stroke-width="4" stroke-linecap="round" stroke-linejoin="round"/>
  <circle cx="44" cy="20" r="3.5" fill="#58F2C2"/>
  <path d="M18 46 H46" stroke="#58F2C2" stroke-width="2.5" stroke-linecap="round"/>
</svg>
)svg";

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
    {
        std::lock_guard<std::mutex> lock(mMutex);
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

    pl::input::registerKeyCallback(&FrameGenMod::onKeyEvent);

    modLogger().info("{} v{} loaded (preset: {}, camera: {})", kModName,
                     kModVersion, presetName(initialCfg.preset),
                     cameraSmoothingModeName(initialCfg.cameraSmoothingMode));
    return true;
}

bool FrameGenMod::enable(pl::mod::ModContext &context) {
    {
        std::lock_guard<std::mutex> lock(mMutex);
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

    std::string ownerModId;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        ownerModId = mModId;
    }

    pl::modmenu::ModuleBuilder builder(kModuleId, kModName);
    builder.modId(ownerModId)
        .description("Frame Generation & Camera Smoothing — motion-compensated "
                     "temporal synthesis, sub-frame camera smoothing, and "
                     "adaptive frame pacing.")
        .defaultEnabled(true)
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

    syncQuickButton(cfg.showQuickButton);
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
    syncQuickButton(false);
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

void FrameGenMod::onQuickButtonEvent(std::string_view buttonId,
                                     pl::modmenu::ButtonEvent eventType,
                                     float /*value*/) {
    if (buttonId != kQuickButtonId) {
        return;
    }
    if (eventType == pl::modmenu::ButtonEvent::Click) {
        instance().cyclePresetInGame();
    }
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
    const FrameGenConfig cfg = configSnapshot();
    const bool enabled = mModuleEnabled.load(std::memory_order_relaxed);

    CameraSmoother::instance().updateConfig(cfg, enabled);
    GlFrameInterpolator::instance().updateConfig(cfg);
    syncQuickButton(cfg.showQuickButton && enabled);

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

void FrameGenMod::syncQuickButton(bool shouldShow) {
    const bool currentlyRegistered =
        mQuickButtonRegistered.load(std::memory_order_relaxed);
    if (shouldShow && !currentlyRegistered) {
        std::string ownerModId;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            ownerModId = mModId;
        }

        pl::modmenu::ButtonInfo btn{};
        btn.buttonId = kQuickButtonId;
        btn.displayName = "LeviFrameGen Preset";
        btn.moduleId = kModuleId;
        btn.modId = ownerModId;
        btn.label = "FG";
        btn.behavior = pl::modmenu::ButtonBehavior::Click;
        btn.defaultVisible = true;
        btn.stylePreset = pl::modmenu::ButtonStylePreset::Accent;
        btn.normalBgColor = 0xCC121B24U;
        btn.activeBgColor = 0xFF38E8B0U;
        btn.borderColor = 0xAA38E8B0U;
        btn.textColor = 0xFF58F2C2U;
        btn.activeTextColor = 0xFF0A1016U;
        btn.iconFormat = pl::modmenu::ButtonIconFormat::Svg;
        btn.hideLabelWhenIconPresent = false;
        btn.iconData.assign(
            reinterpret_cast<const unsigned char *>(kQuickButtonSvgIcon),
            reinterpret_cast<const unsigned char *>(kQuickButtonSvgIcon) +
                std::char_traits<char>::length(kQuickButtonSvgIcon));
        btn.onEvent = &FrameGenMod::onQuickButtonEvent;

        if (HudRenderer::tryRegisterButton(btn)) {
            mQuickButtonRegistered.store(true, std::memory_order_relaxed);
        }
    } else if (!shouldShow && currentlyRegistered) {
        HudRenderer::tryUnregisterButton(kQuickButtonId);
        mQuickButtonRegistered.store(false, std::memory_order_relaxed);
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
