#include "FrameGenConfig.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include <pl/ModMenuConfig.hpp>

namespace framegen {
namespace {

bool parseBoolValue(std::string_view text, bool fallback) noexcept {
    if (text == "true" || text == "1" || text == "on" || text == "yes" ||
        text == "enabled" || text == "TRUE" || text == "ON") {
        return true;
    }
    if (text == "false" || text == "0" || text == "off" || text == "no" ||
        text == "disabled" || text == "FALSE" || text == "OFF") {
        return false;
    }
    return fallback;
}

int parseIntValue(std::string_view text, int fallback) noexcept {
    if (text.empty()) {
        return fallback;
    }
    std::string buf(text);
    char *end = nullptr;
    errno = 0;
    const long val = std::strtol(buf.c_str(), &end, 10);
    if (end == buf.c_str() || errno == ERANGE) {
        return fallback;
    }
    return static_cast<int>(val);
}

float parseFloatValue(std::string_view text, float fallback) noexcept {
    if (text.empty()) {
        return fallback;
    }
    std::string buf(text);
    char *end = nullptr;
    errno = 0;
    const float val = std::strtof(buf.c_str(), &end);
    if (end == buf.c_str() || errno == ERANGE || !std::isfinite(val)) {
        return fallback;
    }
    return val;
}

std::string formatFloat(float value, int decimals = 2) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%.*f", decimals,
                  static_cast<double>(value));
    return std::string(buf);
}

QualityPreset parsePresetValue(std::string_view text,
                               QualityPreset fallback) noexcept {
    if (text == "Power Saver" || text == "power_saver") {
        return QualityPreset::PowerSaver;
    }
    if (text == "Balanced" || text == "balanced") {
        return QualityPreset::Balanced;
    }
    if (text == "Smooth" || text == "smooth") {
        return QualityPreset::Smooth;
    }
    if (text == "Ultra" || text == "ultra") {
        return QualityPreset::Ultra;
    }
    if (text == "Custom" || text == "custom") {
        return QualityPreset::Custom;
    }
    const int idx = parseIntValue(text, static_cast<int>(fallback));
    return static_cast<QualityPreset>(std::clamp(idx, 0, 4));
}

FrameGenMode parseFrameGenModeValue(std::string_view text,
                                    FrameGenMode fallback) noexcept {
    if (text == "Off" || text == "off") {
        return FrameGenMode::Off;
    }
    if (text == "Single-Swap Motion Synthesis" || text == "single_swap" ||
        text == "Single-Swap") {
        return FrameGenMode::SingleSwapSynthesis;
    }
    if (text == "Multi-Swap Frame Interpolation" || text == "multi_swap" ||
        text == "Multi-Swap") {
        return FrameGenMode::MultiSwapInterpolation;
    }
    const int idx = parseIntValue(text, static_cast<int>(fallback));
    return static_cast<FrameGenMode>(std::clamp(idx, 0, 2));
}

CameraSmoothingMode parseCameraModeValue(
    std::string_view text, CameraSmoothingMode fallback) noexcept {
    if (text == "Off" || text == "off") {
        return CameraSmoothingMode::Off;
    }
    if (text == "Exponential (EMA)" || text == "ema" || text == "Exponential") {
        return CameraSmoothingMode::Exponential;
    }
    if (text == "Critically Damped Spring" || text == "spring" ||
        text == "Spring") {
        return CameraSmoothingMode::CriticallyDampedSpring;
    }
    if (text == "Adaptive Smart (1-Euro)" || text == "adaptive" ||
        text == "Adaptive Smart") {
        return CameraSmoothingMode::AdaptiveSmart;
    }
    const int idx = parseIntValue(text, static_cast<int>(fallback));
    return static_cast<CameraSmoothingMode>(std::clamp(idx, 0, 3));
}

HudStyle parseHudStyleValue(std::string_view text, HudStyle fallback) noexcept {
    if (text == "Compact" || text == "compact") {
        return HudStyle::Compact;
    }
    if (text == "Detailed" || text == "detailed") {
        return HudStyle::Detailed;
    }
    if (text == "Minimal" || text == "minimal") {
        return HudStyle::Minimal;
    }
    const int idx = parseIntValue(text, static_cast<int>(fallback));
    return static_cast<HudStyle>(std::clamp(idx, 0, 2));
}

// Lightweight flat JSON object parser for config.json persistence
bool parseFlatJsonObject(
    std::string_view json,
    std::unordered_map<std::string, std::string> &outMap) {
    std::size_t pos = 0;
    const std::size_t n = json.size();
    auto skipWs = [&]() {
        while (pos < n &&
               std::isspace(static_cast<unsigned char>(json[pos]))) {
            ++pos;
        }
    };

    skipWs();
    if (pos >= n || json[pos] != '{') {
        return false;
    }
    ++pos;

    while (pos < n) {
        skipWs();
        if (pos < n && json[pos] == '}') {
            ++pos;
            return true;
        }
        if (pos >= n || json[pos] != '"') {
            return false;
        }
        ++pos;
        std::string key;
        while (pos < n && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < n) {
                ++pos;
            }
            key.push_back(json[pos++]);
        }
        if (pos >= n || json[pos] != '"') {
            return false;
        }
        ++pos;

        skipWs();
        if (pos >= n || json[pos] != ':') {
            return false;
        }
        ++pos;
        skipWs();

        std::string val;
        if (pos < n && json[pos] == '"') {
            ++pos;
            while (pos < n && json[pos] != '"') {
                if (json[pos] == '\\' && pos + 1 < n) {
                    ++pos;
                }
                val.push_back(json[pos++]);
            }
            if (pos >= n || json[pos] != '"') {
                return false;
            }
            ++pos;
        } else {
            while (pos < n && json[pos] != ',' && json[pos] != '}' &&
                   !std::isspace(static_cast<unsigned char>(json[pos]))) {
                val.push_back(json[pos++]);
            }
        }

        outMap[std::move(key)] = std::move(val);
        skipWs();
        if (pos < n && json[pos] == ',') {
            ++pos;
        }
    }
    return false;
}

} // namespace

const char *presetName(QualityPreset preset) noexcept {
    switch (preset) {
    case QualityPreset::PowerSaver:
        return "Power Saver";
    case QualityPreset::Balanced:
        return "Balanced";
    case QualityPreset::Smooth:
        return "Smooth";
    case QualityPreset::Ultra:
        return "Ultra";
    case QualityPreset::Custom:
        return "Custom";
    }
    return "Balanced";
}

const char *frameGenModeName(FrameGenMode mode) noexcept {
    switch (mode) {
    case FrameGenMode::Off:
        return "Off";
    case FrameGenMode::SingleSwapSynthesis:
        return "Single-Swap Motion Synthesis";
    case FrameGenMode::MultiSwapInterpolation:
        return "Multi-Swap Frame Interpolation";
    }
    return "Single-Swap Motion Synthesis";
}

const char *cameraSmoothingModeName(CameraSmoothingMode mode) noexcept {
    switch (mode) {
    case CameraSmoothingMode::Off:
        return "Off";
    case CameraSmoothingMode::Exponential:
        return "Exponential (EMA)";
    case CameraSmoothingMode::CriticallyDampedSpring:
        return "Critically Damped Spring";
    case CameraSmoothingMode::AdaptiveSmart:
        return "Adaptive Smart (1-Euro)";
    }
    return "Adaptive Smart (1-Euro)";
}

const char *hudStyleName(HudStyle style) noexcept {
    switch (style) {
    case HudStyle::Compact:
        return "Compact";
    case HudStyle::Detailed:
        return "Detailed";
    case HudStyle::Minimal:
        return "Minimal";
    }
    return "Detailed";
}

void FrameGenConfig::normalize() noexcept {
    schemaVersion = 2;
    preset = static_cast<QualityPreset>(
        std::clamp(static_cast<int>(preset), 0, 4));
    frameGenMode = static_cast<FrameGenMode>(
        std::clamp(static_cast<int>(frameGenMode), 0, 2));
    frameGenMultiplier = std::clamp(frameGenMultiplier, 2, 4);
    blendStrength = std::clamp(
        std::isfinite(blendStrength) ? blendStrength : 0.65f, 0.0f, 1.0f);
    motionScale = std::clamp(
        std::isfinite(motionScale) ? motionScale : 1.0f, 0.0f, 2.0f);
    hudProtection = std::clamp(
        std::isfinite(hudProtection) ? hudProtection : 0.88f, 0.0f, 1.0f);
    sceneCutThreshold = std::clamp(
        std::isfinite(sceneCutThreshold) ? sceneCutThreshold : 42.0f, 10.0f,
        120.0f);

    cameraSmoothingMode = static_cast<CameraSmoothingMode>(
        std::clamp(static_cast<int>(cameraSmoothingMode), 0, 3));
    cameraSmoothness = std::clamp(
        std::isfinite(cameraSmoothness) ? cameraSmoothness : 0.55f, 0.0f,
        0.95f);
    cameraResponsiveness = std::clamp(
        std::isfinite(cameraResponsiveness) ? cameraResponsiveness : 1.05f,
        0.1f, 2.5f);
    microDeadzone = std::clamp(
        std::isfinite(microDeadzone) ? microDeadzone : 0.025f, 0.0f, 0.5f);
    spikeFilterDeg = std::clamp(
        std::isfinite(spikeFilterDeg) ? spikeFilterDeg : 75.0f, 10.0f, 180.0f);

    if (targetFps != 0) {
        targetFps = std::clamp(targetFps, 30, 240);
    }

    hudStyle = static_cast<HudStyle>(
        std::clamp(static_cast<int>(hudStyle), 0, 2));
    hudOpacity = std::clamp(hudOpacity, 20, 100);
    hudPosX = std::clamp(std::isfinite(hudPosX) ? hudPosX : 16.0f, 0.0f,
                         4096.0f);
    hudPosY = std::clamp(std::isfinite(hudPosY) ? hudPosY : 16.0f, 0.0f,
                         4096.0f);
}

void FrameGenConfig::applyPreset(QualityPreset newPreset) noexcept {
    preset = newPreset;
    switch (newPreset) {
    case QualityPreset::PowerSaver:
        frameGenMode = FrameGenMode::SingleSwapSynthesis;
        frameGenMultiplier = 2;
        blendStrength = 0.45f;
        motionScale = 0.85f;
        hudProtection = 0.92f;
        sceneCutThreshold = 35.0f;
        cameraSmoothingMode = CameraSmoothingMode::Exponential;
        cameraSmoothness = 0.40f;
        cameraResponsiveness = 1.15f;
        microDeadzone = 0.02f;
        spikeFilterDeg = 80.0f;
        fovScaling = true;
        autoTune = true;
        break;

    case QualityPreset::Balanced:
        frameGenMode = FrameGenMode::SingleSwapSynthesis;
        frameGenMultiplier = 2;
        blendStrength = 0.65f;
        motionScale = 1.0f;
        hudProtection = 0.88f;
        sceneCutThreshold = 42.0f;
        cameraSmoothingMode = CameraSmoothingMode::AdaptiveSmart;
        cameraSmoothness = 0.55f;
        cameraResponsiveness = 1.05f;
        microDeadzone = 0.025f;
        spikeFilterDeg = 75.0f;
        fovScaling = true;
        autoTune = true;
        break;

    case QualityPreset::Smooth:
        frameGenMode = FrameGenMode::SingleSwapSynthesis;
        frameGenMultiplier = 2;
        blendStrength = 0.78f;
        motionScale = 1.05f;
        hudProtection = 0.86f;
        sceneCutThreshold = 48.0f;
        cameraSmoothingMode = CameraSmoothingMode::AdaptiveSmart;
        cameraSmoothness = 0.68f;
        cameraResponsiveness = 0.95f;
        microDeadzone = 0.03f;
        spikeFilterDeg = 70.0f;
        fovScaling = true;
        autoTune = true;
        break;

    case QualityPreset::Ultra:
        frameGenMode = FrameGenMode::MultiSwapInterpolation;
        frameGenMultiplier = 2;
        blendStrength = 0.82f;
        motionScale = 1.10f;
        hudProtection = 0.85f;
        sceneCutThreshold = 55.0f;
        cameraSmoothingMode = CameraSmoothingMode::AdaptiveSmart;
        cameraSmoothness = 0.62f;
        cameraResponsiveness = 1.15f;
        microDeadzone = 0.03f;
        spikeFilterDeg = 65.0f;
        fovScaling = true;
        autoTune = true;
        break;

    case QualityPreset::Custom:
        break;
    }
    normalize();
}

ConfigApplyResult applyConfigKeyValue(FrameGenConfig &config,
                                      std::string_view key,
                                      std::string_view value) noexcept {
    if (key == keys::kActionResetDefaults) {
        const float savedX = config.hudPosX;
        const float savedY = config.hudPosY;
        config = FrameGenConfig{};
        config.hudPosX = savedX;
        config.hudPosY = savedY;
        config.normalize();
        return ConfigApplyResult::TriggeredResetDefaults;
    }

    if (key == keys::kActionResetTemporal) {
        return ConfigApplyResult::TriggeredResetTemporal;
    }

    if (key == keys::kActionCyclePreset) {
        const int nextIdx = (static_cast<int>(config.preset) + 1) % 4;
        config.applyPreset(static_cast<QualityPreset>(nextIdx));
        return ConfigApplyResult::TriggeredCyclePreset;
    }

    if (key == keys::kPreset) {
        const QualityPreset p = parsePresetValue(value, config.preset);
        config.applyPreset(p);
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kFrameGenMode) {
        config.frameGenMode =
            parseFrameGenModeValue(value, config.frameGenMode);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kFrameGenMultiplier) {
        config.frameGenMultiplier =
            parseIntValue(value, config.frameGenMultiplier);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kBlendStrength) {
        config.blendStrength = parseFloatValue(value, config.blendStrength);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kMotionScale) {
        config.motionScale = parseFloatValue(value, config.motionScale);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kHudProtection) {
        config.hudProtection = parseFloatValue(value, config.hudProtection);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kSceneCutThreshold) {
        config.sceneCutThreshold =
            parseFloatValue(value, config.sceneCutThreshold);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kCameraSmoothingMode) {
        config.cameraSmoothingMode =
            parseCameraModeValue(value, config.cameraSmoothingMode);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kCameraSmoothness) {
        config.cameraSmoothness =
            parseFloatValue(value, config.cameraSmoothness);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kCameraResponsiveness) {
        config.cameraResponsiveness =
            parseFloatValue(value, config.cameraResponsiveness);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kMicroDeadzone) {
        config.microDeadzone = parseFloatValue(value, config.microDeadzone);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kSpikeFilterDeg) {
        config.spikeFilterDeg = parseFloatValue(value, config.spikeFilterDeg);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kFovScaling) {
        config.fovScaling = parseBoolValue(value, config.fovScaling);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kAutoTune) {
        config.autoTune = parseBoolValue(value, config.autoTune);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kTargetFps) {
        config.targetFps = parseIntValue(value, config.targetFps);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kShowHud) {
        config.showHud = parseBoolValue(value, config.showHud);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kHudStyle) {
        config.hudStyle = parseHudStyleValue(value, config.hudStyle);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kHudOpacity) {
        config.hudOpacity = parseIntValue(value, config.hudOpacity);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kHudPosX || key == "hudPosX") {
        config.hudPosX = parseFloatValue(value, config.hudPosX);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kHudPosY || key == "hudPosY") {
        config.hudPosY = parseFloatValue(value, config.hudPosY);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kShowQuickButton) {
        config.showQuickButton = parseBoolValue(value, config.showQuickButton);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    return ConfigApplyResult::Unchanged;
}

void appendV1ModuleConfigs(pl::modmenu::ModuleBuilder &builder,
                           const FrameGenConfig &config) {
    builder
        .config(keys::kPreset,
                "Preset (0=PowerSaver, 1=Balanced, 2=Smooth, 3=Ultra, 4=Custom)",
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(static_cast<int>(config.preset)), "0", "4")
        .config(keys::kFrameGenMode,
                "FrameGen Mode (0=Off, 1=Single-Swap Synthesis, 2=Multi-Swap)",
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(static_cast<int>(config.frameGenMode)), "0", "2")
        .config(keys::kFrameGenMultiplier,
                "Frame Multiplier (2x - 4x)",
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(config.frameGenMultiplier), "2", "4")
        .config(keys::kBlendStrength,
                "Temporal Synthesis Strength",
                pl::modmenu::ConfigType::SliderFloat,
                formatFloat(config.blendStrength, 2), "0.00", "1.00")
        .config(keys::kMotionScale,
                "Optical Motion Vector Scale",
                pl::modmenu::ConfigType::SliderFloat,
                formatFloat(config.motionScale, 2), "0.00", "2.00")
        .config(keys::kHudProtection,
                "HUD / Crosshair Edge Protection",
                pl::modmenu::ConfigType::SliderFloat,
                formatFloat(config.hudProtection, 2), "0.00", "1.00")
        .config(keys::kSceneCutThreshold,
                "Scene-Cut Reset Threshold (deg/frame)",
                pl::modmenu::ConfigType::SliderFloat,
                formatFloat(config.sceneCutThreshold, 1), "10.0", "120.0")
        .config(keys::kCameraSmoothingMode,
                "Camera Smoothing (0=Off, 1=EMA, 2=Spring, 3=Adaptive Smart)",
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(static_cast<int>(config.cameraSmoothingMode)),
                "0", "3")
        .config(keys::kCameraSmoothness,
                "Camera Smoothing Strength",
                pl::modmenu::ConfigType::SliderFloat,
                formatFloat(config.cameraSmoothness, 2), "0.00", "0.95")
        .config(keys::kCameraResponsiveness,
                "Camera Flick Responsiveness",
                pl::modmenu::ConfigType::SliderFloat,
                formatFloat(config.cameraResponsiveness, 2), "0.10", "2.50")
        .config(keys::kMicroDeadzone,
                "Touch Micro-Jitter Deadzone (deg)",
                pl::modmenu::ConfigType::SliderFloat,
                formatFloat(config.microDeadzone, 3), "0.000", "0.500")
        .config(keys::kSpikeFilterDeg,
                "Touch Turn-Spike Clamp (deg)",
                pl::modmenu::ConfigType::SliderFloat,
                formatFloat(config.spikeFilterDeg, 1), "10.0", "180.0")
        .config(keys::kFovScaling,
                "FOV-Aware Zoom Sensitivity Scaling",
                pl::modmenu::ConfigType::Toggle,
                config.fovScaling ? "true" : "false")
        .config(keys::kAutoTune,
                "Smart Auto-Tune Frame Budget Guard",
                pl::modmenu::ConfigType::Toggle,
                config.autoTune ? "true" : "false")
        .config(keys::kTargetFps,
                "Frame Pacer Target FPS (0=Unlimited)",
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(config.targetFps), "0", "240")
        .config(keys::kShowHud,
                "Show In-Game Telemetry HUD",
                pl::modmenu::ConfigType::Toggle,
                config.showHud ? "true" : "false")
        .config(keys::kHudStyle,
                "HUD Style (0=Compact, 1=Detailed, 2=Minimal)",
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(static_cast<int>(config.hudStyle)), "0", "2")
        .config(keys::kHudOpacity,
                "HUD Card Opacity (%)",
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(config.hudOpacity), "20", "100");
}

std::string buildV2ConfigSchemaJson(const FrameGenConfig &config,
                                    const RuntimeTelemetrySnapshot &telemetry) {
    using pl::modmenu::ConfigChoiceStyleV2;
    using pl::modmenu::ConfigConditionOpV2;
    using pl::modmenu::ConfigConditionV2;
    using pl::modmenu::ConfigControlTypeV2;
    using pl::modmenu::ConfigNodeV2;
    using pl::modmenu::ConfigOptionV2;
    using pl::modmenu::ConfigSchemaBuilder;

    char fpsInfoBuf[180]{};
    std::snprintf(fpsInfoBuf, sizeof(fpsInfoBuf),
                  "Native: %.1f FPS | Effective: %.1f FPS (%dx) | 1%% Low: %.1f | "
                  "Frame: %.2f ms (Jitter: %.2f ms)",
                  static_cast<double>(telemetry.realFps),
                  static_cast<double>(telemetry.effectiveFps),
                  telemetry.activeMultiplier,
                  static_cast<double>(telemetry.onePercentLowFps),
                  static_cast<double>(telemetry.frameTimeMs),
                  static_cast<double>(telemetry.jitterMs));

    char camInfoBuf[180]{};
    std::snprintf(camInfoBuf, sizeof(camInfoBuf),
                  "Mode: %s | FOV: %.1f deg | Turn Vel: (%.2f, %.2f) deg/f | "
                  "Motion UV: (%.4f, %.4f)",
                  cameraSmoothingModeName(config.cameraSmoothingMode),
                  static_cast<double>(telemetry.currentFovDeg),
                  static_cast<double>(telemetry.cameraYawVelDeg),
                  static_cast<double>(telemetry.cameraPitchVelDeg),
                  static_cast<double>(telemetry.lastMotionU),
                  static_cast<double>(telemetry.lastMotionV));

    char hookInfoBuf[180]{};
    std::snprintf(hookInfoBuf, sizeof(hookInfoBuf),
                  "EGL Swap: %s | TurnDelta: %s | CameraAPI FOV: %s | "
                  "Perspective: %s",
                  telemetry.eglHooked ? "Active" : "Standby",
                  telemetry.turnDeltaHooked ? "Hooked" : "Fallback",
                  telemetry.fovHooked ? "Hooked" : "Default 70",
                  telemetry.perspectiveHooked ? "Hooked" : "Auto");

    const std::vector<ConfigConditionV2> fgEnabledCond = {
        {keys::kFrameGenMode, ConfigConditionOpV2::NotEquals, "0"}
    };
    const std::vector<ConfigConditionV2> fgMultiSwapCond = {
        {keys::kFrameGenMode, ConfigConditionOpV2::Equals, "2"}
    };
    const std::vector<ConfigConditionV2> camEnabledCond = {
        {keys::kCameraSmoothingMode, ConfigConditionOpV2::NotEquals, "0"}
    };
    const std::vector<ConfigConditionV2> hudEnabledCond = {
        {keys::kShowHud, ConfigConditionOpV2::Truthy, "true"}
    };

    ConfigSchemaBuilder schema;
    schema.defaultCategory("general")
        .category("general", "Presets & Telemetry",
                  "Quick profiles, live FPS/camera diagnostics, and actions.")
        .category("framegen", "Frame Generation",
                  "Motion-compensated GPU frame synthesis and interpolation.")
        .category("camera", "Camera Smoothing",
                  "Sub-frame camera smoothing with 100% angle conservation.")
        .category("pacing", "Pacing & HUD",
                  "Frame pacing, Smart Auto-Tune, and draggable HUD overlay.");

    // --- Category: general ---
    {
        ConfigNodeV2 presetNode{};
        presetNode.id = keys::kPreset;
        presetNode.key = keys::kPreset;
        presetNode.category = "general";
        presetNode.section = "Quality Profile";
        presetNode.title = "Performance & Smoothness Preset";
        presetNode.description =
            "Automatically tunes Frame Generation and Camera Smoothing.";
        presetNode.type = ConfigControlTypeV2::Choice;
        presetNode.choiceStyle = ConfigChoiceStyleV2::Segmented;
        presetNode.defaultValue = "1";
        presetNode.currentValue =
            std::to_string(static_cast<int>(config.preset));
        presetNode.hasCurrentValue = true;
        presetNode.options = {
            {"0", "Power Saver", "Minimal GPU overhead, 60 FPS target", "", false, "", false},
            {"1", "Balanced", "Recommended single-swap synthesis + Smart Camera", "", false, "", false},
            {"2", "Smooth", "High-strength synthesis + silky camera filter", "", false, "", false},
            {"3", "Ultra", "Multi-swap interpolation + ultra-fast response", "", false, "", false},
            {"4", "Custom", "User-customized settings", "", false, "", false},
        };
        schema.node(std::move(presetNode));
    }

    auto addInfoNode = [&](const char *id, const char *title,
                           const char *desc) {
        ConfigNodeV2 n{};
        n.id = id;
        n.key = id;
        n.category = "general";
        n.section = "Live Telemetry";
        n.title = title;
        n.description = desc;
        n.type = ConfigControlTypeV2::Info;
        n.currentValue = desc;
        n.hasCurrentValue = true;
        schema.node(std::move(n));
    };
    addInfoNode("fps_telemetry", "Frame Rate & Pacing", fpsInfoBuf);
    addInfoNode("camera_telemetry", "Camera & Optical Flow", camInfoBuf);
    addInfoNode("hook_telemetry", "Native Engine Hooks", hookInfoBuf);

    auto addButtonNode = [&](const char *key, const char *title,
                             const char *desc, const char *label) {
        ConfigNodeV2 n{};
        n.id = key;
        n.key = key;
        n.category = "general";
        n.section = "Quick Actions";
        n.title = title;
        n.description = desc;
        n.type = ConfigControlTypeV2::Button;
        n.placeholder = label;
        n.actionValue = "1";
        schema.node(std::move(n));
    };
    addButtonNode(keys::kActionCyclePreset, "Cycle Quality Preset",
                  "Cycles through Power Saver, Balanced, Smooth, and Ultra.",
                  "Cycle Preset");
    addButtonNode(keys::kActionResetTemporal, "Flush Temporal History",
                  "Clears buffered frame textures and camera velocity state.",
                  "Flush History");
    addButtonNode(keys::kActionResetDefaults, "Restore Default Settings",
                  "Resets all settings back to Balanced defaults.",
                  "Reset Defaults");

    // --- Category: framegen ---
    {
        ConfigNodeV2 modeNode{};
        modeNode.id = keys::kFrameGenMode;
        modeNode.key = keys::kFrameGenMode;
        modeNode.category = "framegen";
        modeNode.section = "Synthesis Mode";
        modeNode.title = "Frame Generation Mode";
        modeNode.description =
            "Single-Swap synthesizes motion inside Minecraft's native swap "
            "(zero SurfaceFlinger jitter). Multi-Swap inserts intermediate "
            "swaps via eglPresentationTimeANDROID.";
        modeNode.type = ConfigControlTypeV2::Choice;
        modeNode.choiceStyle = ConfigChoiceStyleV2::Radio;
        modeNode.defaultValue = "1";
        modeNode.currentValue =
            std::to_string(static_cast<int>(config.frameGenMode));
        modeNode.hasCurrentValue = true;
        modeNode.options = {
            {"0", "Off", "Pass-through rendering (Camera Smoothing still active)", "", false, "", false},
            {"1", "Single-Swap Motion Synthesis", "Zero-jitter optical-flow temporal synthesis (Recommended)", "", false, "", false},
            {"2", "Multi-Swap Frame Interpolation", "2x-4x intermediate frame insertion via presentation timestamps", "", false, "", false},
        };
        schema.node(std::move(modeNode));
    }

    auto addSliderInt = [&](const char *key, const char *cat, const char *sec,
                            const char *title, const char *desc, int cur,
                            int defVal, int minV, int maxV, int stepV,
                            const char *unit,
                            const std::vector<ConfigConditionV2> &vis,
                            const std::vector<ConfigConditionV2> &en) {
        ConfigNodeV2 n{};
        n.id = key;
        n.key = key;
        n.category = cat;
        n.section = sec;
        n.title = title;
        n.description = desc;
        n.type = ConfigControlTypeV2::SliderInt;
        n.defaultValue = std::to_string(defVal);
        n.currentValue = std::to_string(cur);
        n.hasCurrentValue = true;
        n.minValue = std::to_string(minV);
        n.maxValue = std::to_string(maxV);
        n.step = std::to_string(stepV);
        n.unit = unit;
        n.visibleWhen = vis;
        n.enabledWhen = en;
        schema.node(std::move(n));
    };

    auto addSliderFloat =
        [&](const char *key, const char *cat, const char *sec,
            const char *title, const char *desc, float cur, float defVal,
            float minV, float maxV, float stepV, int decimals, const char *unit,
            const std::vector<ConfigConditionV2> &vis,
            const std::vector<ConfigConditionV2> &en) {
            ConfigNodeV2 n{};
            n.id = key;
            n.key = key;
            n.category = cat;
            n.section = sec;
            n.title = title;
            n.description = desc;
            n.type = ConfigControlTypeV2::SliderFloat;
            n.defaultValue = formatFloat(defVal, decimals);
            n.currentValue = formatFloat(cur, decimals);
            n.hasCurrentValue = true;
            n.minValue = formatFloat(minV, decimals);
            n.maxValue = formatFloat(maxV, decimals);
            n.step = formatFloat(stepV, decimals);
            n.unit = unit;
            n.visibleWhen = vis;
            n.enabledWhen = en;
            schema.node(std::move(n));
        };

    auto addToggle = [&](const char *key, const char *cat, const char *sec,
                         const char *title, const char *desc, bool cur,
                         bool defVal,
                         const std::vector<ConfigConditionV2> &vis,
                         const std::vector<ConfigConditionV2> &en) {
        ConfigNodeV2 n{};
        n.id = key;
        n.key = key;
        n.category = cat;
        n.section = sec;
        n.title = title;
        n.description = desc;
        n.type = ConfigControlTypeV2::Toggle;
        n.defaultValue = defVal ? "true" : "false";
        n.currentValue = cur ? "true" : "false";
        n.hasCurrentValue = true;
        n.visibleWhen = vis;
        n.enabledWhen = en;
        schema.node(std::move(n));
    };

    addSliderInt(keys::kFrameGenMultiplier, "framegen", "Synthesis Tuning",
                 "Target Frame Multiplier",
                 "Number of total frames presented per native game frame.",
                 config.frameGenMultiplier, 2, 2, 4, 1, "x", fgMultiSwapCond,
                 fgEnabledCond);
    addSliderFloat(keys::kBlendStrength, "framegen", "Synthesis Tuning",
                   "Temporal Synthesis Strength",
                   "Controls optical-flow temporal synthesis weight.",
                   config.blendStrength, 0.65f, 0.0f, 1.0f, 0.05f, 2, "",
                   fgEnabledCond, fgEnabledCond);
    addSliderFloat(keys::kMotionScale, "framegen", "Synthesis Tuning",
                   "Optical Motion Vector Scale",
                   "Scales camera-to-screen optical flow displacement.",
                   config.motionScale, 1.0f, 0.0f, 2.0f, 0.05f, 2, "x",
                   fgEnabledCond, fgEnabledCond);
    addSliderFloat(keys::kHudProtection, "framegen", "Artifact Protection",
                   "HUD & Crosshair Edge Protection",
                   "Keeps static HUD, crosshair, and hotbar razor-sharp.",
                   config.hudProtection, 0.88f, 0.0f, 1.0f, 0.02f, 2, "",
                   fgEnabledCond, fgEnabledCond);
    addSliderFloat(keys::kSceneCutThreshold, "framegen", "Artifact Protection",
                   "Scene-Cut Reset Threshold",
                   "Angular velocity threshold that flushes temporal history.",
                   config.sceneCutThreshold, 42.0f, 10.0f, 120.0f, 2.0f, 1,
                   "deg/f", fgEnabledCond, fgEnabledCond);

    // --- Category: camera ---
    {
        ConfigNodeV2 camModeNode{};
        camModeNode.id = keys::kCameraSmoothingMode;
        camModeNode.key = keys::kCameraSmoothingMode;
        camModeNode.category = "camera";
        camModeNode.section = "Filter Algorithm";
        camModeNode.title = "Camera Smoothing Algorithm";
        camModeNode.description =
            "Adaptive Smart suppresses slow-pan micro-jitter while keeping "
            "fast combat flicks zero-latency.";
        camModeNode.type = ConfigControlTypeV2::Choice;
        camModeNode.choiceStyle = ConfigChoiceStyleV2::Radio;
        camModeNode.defaultValue = "3";
        camModeNode.currentValue =
            std::to_string(static_cast<int>(config.cameraSmoothingMode));
        camModeNode.hasCurrentValue = true;
        camModeNode.options = {
            {"0", "Off", "Raw unsmoothed camera input", "", false, "", false},
            {"1", "Exponential (EMA)", "Frame-rate invariant exponential filter", "", false, "", false},
            {"2", "Critically Damped Spring", "Inertial zero-overshoot spring damper", "", false, "", false},
            {"3", "Adaptive Smart (1-Euro)", "Velocity-adaptive cinematic + snappy filter (Recommended)", "", false, "", false},
        };
        schema.node(std::move(camModeNode));
    }

    addSliderFloat(keys::kCameraSmoothness, "camera", "Response Tuning",
                   "Camera Smoothing Strength",
                   "Higher values produce silkier cinematic camera pans.",
                   config.cameraSmoothness, 0.55f, 0.0f, 0.95f, 0.05f, 2, "",
                   camEnabledCond, camEnabledCond);
    addSliderFloat(keys::kCameraResponsiveness, "camera", "Response Tuning",
                   "Fast-Turn Responsiveness Gain",
                   "Controls how fast the filter snaps during rapid flicks.",
                   config.cameraResponsiveness, 1.05f, 0.10f, 2.50f, 0.05f, 2,
                   "x", camEnabledCond, camEnabledCond);
    addSliderFloat(keys::kMicroDeadzone, "camera", "Touch & Gyro Conditioning",
                   "Touch Micro-Jitter Deadzone",
                   "Suppresses direction-reversing touch digitizer noise.",
                   config.microDeadzone, 0.025f, 0.0f, 0.50f, 0.005f, 3, "deg",
                   camEnabledCond, camEnabledCond);
    addSliderFloat(keys::kSpikeFilterDeg, "camera",
                   "Touch & Gyro Conditioning", "Turn-Spike Clamp",
                   "Clamps single-frame multi-touch jump glitches.",
                   config.spikeFilterDeg, 75.0f, 10.0f, 180.0f, 5.0f, 1,
                   "deg/f", camEnabledCond, camEnabledCond);
    addToggle(keys::kFovScaling, "camera", "Touch & Gyro Conditioning",
              "FOV-Aware Zoom Sensitivity Scaling",
              "Automatically steadies camera smoothing when zoomed in.",
              config.fovScaling, true, camEnabledCond, camEnabledCond);

    // --- Category: pacing & HUD ---
    addToggle(keys::kAutoTune, "pacing", "Frame Pacing",
              "Smart Auto-Tune Budget Guard",
              "Adapts synthesis workload if GPU frame times spike.",
              config.autoTune, true, {}, {});
    addSliderInt(keys::kTargetFps, "pacing", "Frame Pacing",
                 "Frame Pacer Target FPS (0 = Unlimited)",
                 "Set to 0 for native VSync, or set a custom FPS target.",
                 config.targetFps, 0, 0, 240, 5, "FPS", {}, {});
    addToggle(keys::kShowHud, "pacing", "HUD Overlay",
              "Show In-Game Telemetry HUD",
              "Displays live FPS, Effective FPS, Jitter, and Camera status.",
              config.showHud, true, {}, {});

    {
        ConfigNodeV2 styleNode{};
        styleNode.id = keys::kHudStyle;
        styleNode.key = keys::kHudStyle;
        styleNode.category = "pacing";
        styleNode.section = "HUD Overlay";
        styleNode.title = "HUD Layout Style";
        styleNode.description = "Select HUD card information density.";
        styleNode.type = ConfigControlTypeV2::Choice;
        styleNode.choiceStyle = ConfigChoiceStyleV2::Segmented;
        styleNode.defaultValue = "1";
        styleNode.currentValue =
            std::to_string(static_cast<int>(config.hudStyle));
        styleNode.hasCurrentValue = true;
        styleNode.visibleWhen = hudEnabledCond;
        styleNode.enabledWhen = hudEnabledCond;
        styleNode.options = {
            {"0", "Compact", "2-line summary card", "", false, "", false},
            {"1", "Detailed", "4-line full telemetry card", "", false, "", false},
            {"2", "Minimal", "Compact single-line FPS badge", "", false, "", false},
        };
        schema.node(std::move(styleNode));
    }

    addSliderInt(keys::kHudOpacity, "pacing", "HUD Overlay",
                 "HUD Card Opacity", "Background opacity of the HUD card.",
                 config.hudOpacity, 85, 20, 100, 5, "%", hudEnabledCond,
                 hudEnabledCond);
    addToggle(keys::kShowQuickButton, "pacing", "HUD Overlay",
              "Show Floating Quick-Preset Button",
              "Displays an on-screen button to cycle presets in-game.",
              config.showQuickButton, true, {}, {});

    return schema.toJson();
}

bool loadConfigFromFile(const std::filesystem::path &path,
                        FrameGenConfig &outConfig) {
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec)) {
        outConfig.normalize();
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        outConfig.normalize();
        return false;
    }

    std::ostringstream ss;
    ss << in.rdbuf();

    std::unordered_map<std::string, std::string> kv;
    if (!parseFlatJsonObject(ss.str(), kv)) {
        outConfig.normalize();
        return false;
    }

    auto getInt = [&](const char *k, int &dst) {
        if (auto it = kv.find(k); it != kv.end()) {
            dst = parseIntValue(it->second, dst);
        }
    };
    auto getFloat = [&](const char *k, float &dst) {
        if (auto it = kv.find(k); it != kv.end()) {
            dst = parseFloatValue(it->second, dst);
        }
    };
    auto getBool = [&](const char *k, bool &dst) {
        if (auto it = kv.find(k); it != kv.end()) {
            dst = parseBoolValue(it->second, dst);
        }
    };

    int presetInt = static_cast<int>(outConfig.preset);
    getInt(keys::kPreset, presetInt);
    outConfig.preset = static_cast<QualityPreset>(presetInt);

    int fgModeInt = static_cast<int>(outConfig.frameGenMode);
    getInt(keys::kFrameGenMode, fgModeInt);
    outConfig.frameGenMode = static_cast<FrameGenMode>(fgModeInt);

    getInt(keys::kFrameGenMultiplier, outConfig.frameGenMultiplier);
    getFloat(keys::kBlendStrength, outConfig.blendStrength);
    getFloat(keys::kMotionScale, outConfig.motionScale);
    getFloat(keys::kHudProtection, outConfig.hudProtection);
    getFloat(keys::kSceneCutThreshold, outConfig.sceneCutThreshold);

    int camModeInt = static_cast<int>(outConfig.cameraSmoothingMode);
    getInt(keys::kCameraSmoothingMode, camModeInt);
    outConfig.cameraSmoothingMode = static_cast<CameraSmoothingMode>(camModeInt);

    getFloat(keys::kCameraSmoothness, outConfig.cameraSmoothness);
    getFloat(keys::kCameraResponsiveness, outConfig.cameraResponsiveness);
    getFloat(keys::kMicroDeadzone, outConfig.microDeadzone);
    getFloat(keys::kSpikeFilterDeg, outConfig.spikeFilterDeg);
    getBool(keys::kFovScaling, outConfig.fovScaling);

    getBool(keys::kAutoTune, outConfig.autoTune);
    getInt(keys::kTargetFps, outConfig.targetFps);

    getBool(keys::kShowHud, outConfig.showHud);
    int hudStyleInt = static_cast<int>(outConfig.hudStyle);
    getInt(keys::kHudStyle, hudStyleInt);
    outConfig.hudStyle = static_cast<HudStyle>(hudStyleInt);

    getInt(keys::kHudOpacity, outConfig.hudOpacity);
    getFloat(keys::kHudPosX, outConfig.hudPosX);
    getFloat(keys::kHudPosY, outConfig.hudPosY);
    getBool(keys::kShowQuickButton, outConfig.showQuickButton);

    outConfig.normalize();
    return true;
}

bool saveConfigToFile(const std::filesystem::path &path,
                      const FrameGenConfig &config) {
    if (path.empty()) {
        return false;
    }

    FrameGenConfig c = config;
    c.normalize();

    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }

    out << "{\n"
        << "  \"schema_version\": " << c.schemaVersion << ",\n"
        << "  \"" << keys::kPreset << "\": " << static_cast<int>(c.preset) << ",\n"
        << "  \"" << keys::kFrameGenMode << "\": " << static_cast<int>(c.frameGenMode) << ",\n"
        << "  \"" << keys::kFrameGenMultiplier << "\": " << c.frameGenMultiplier << ",\n"
        << "  \"" << keys::kBlendStrength << "\": " << formatFloat(c.blendStrength, 4) << ",\n"
        << "  \"" << keys::kMotionScale << "\": " << formatFloat(c.motionScale, 4) << ",\n"
        << "  \"" << keys::kHudProtection << "\": " << formatFloat(c.hudProtection, 4) << ",\n"
        << "  \"" << keys::kSceneCutThreshold << "\": " << formatFloat(c.sceneCutThreshold, 2) << ",\n"
        << "  \"" << keys::kCameraSmoothingMode << "\": " << static_cast<int>(c.cameraSmoothingMode) << ",\n"
        << "  \"" << keys::kCameraSmoothness << "\": " << formatFloat(c.cameraSmoothness, 4) << ",\n"
        << "  \"" << keys::kCameraResponsiveness << "\": " << formatFloat(c.cameraResponsiveness, 4) << ",\n"
        << "  \"" << keys::kMicroDeadzone << "\": " << formatFloat(c.microDeadzone, 4) << ",\n"
        << "  \"" << keys::kSpikeFilterDeg << "\": " << formatFloat(c.spikeFilterDeg, 2) << ",\n"
        << "  \"" << keys::kFovScaling << "\": " << (c.fovScaling ? "true" : "false") << ",\n"
        << "  \"" << keys::kAutoTune << "\": " << (c.autoTune ? "true" : "false") << ",\n"
        << "  \"" << keys::kTargetFps << "\": " << c.targetFps << ",\n"
        << "  \"" << keys::kShowHud << "\": " << (c.showHud ? "true" : "false") << ",\n"
        << "  \"" << keys::kHudStyle << "\": " << static_cast<int>(c.hudStyle) << ",\n"
        << "  \"" << keys::kHudOpacity << "\": " << c.hudOpacity << ",\n"
        << "  \"" << keys::kHudPosX << "\": " << formatFloat(c.hudPosX, 2) << ",\n"
        << "  \"" << keys::kHudPosY << "\": " << formatFloat(c.hudPosY, 2) << ",\n"
        << "  \"" << keys::kShowQuickButton << "\": " << (c.showQuickButton ? "true" : "false") << "\n"
        << "}\n";

    return out.good();
}

} // namespace framegen
