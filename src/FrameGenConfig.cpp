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

int parseMultiplierValue(std::string_view text, int fallback) noexcept {
    if (text == "2" || text == "2x" || text == "2X" || text == "2×") {
        return 2;
    }
    if (text == "3" || text == "3x" || text == "3X" || text == "3×") {
        return 3;
    }
    if (text == "4" || text == "4x" || text == "4X" || text == "4×") {
        return 4;
    }
    return std::clamp(parseIntValue(text, fallback), 2, 4);
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

HudOptimizerMode parseHudOptimizerModeValue(
    std::string_view text, HudOptimizerMode fallback) noexcept {
    if (text == "Off" || text == "off") {
        return HudOptimizerMode::Off;
    }
    if (text == "Balanced" || text == "balanced") {
        return HudOptimizerMode::Balanced;
    }
    if (text == "Ultra Low-Latency" || text == "ultra" ||
        text == "Ultra Low-Latency (Recommended)") {
        return HudOptimizerMode::UltraLowLatency;
    }
    const int idx = parseIntValue(text, static_cast<int>(fallback));
    return static_cast<HudOptimizerMode>(std::clamp(idx, 0, 2));
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
    return "Minimal";
}

const char *hudOptimizerModeName(HudOptimizerMode mode) noexcept {
    switch (mode) {
    case HudOptimizerMode::Off:
        return "Off";
    case HudOptimizerMode::Balanced:
        return "Balanced";
    case HudOptimizerMode::UltraLowLatency:
        return "Ultra Low-Latency";
    }
    return "Ultra Low-Latency";
}

void FrameGenConfig::normalize() noexcept {
    schemaVersion = 3;
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
    if (frameGenOutputFps != 0) {
        frameGenOutputFps = std::clamp(frameGenOutputFps, 30, 240);
    }

    hudOptimizerMode = static_cast<HudOptimizerMode>(
        std::clamp(static_cast<int>(hudOptimizerMode), 0, 2));

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
        frameGenMultiplier = 3;
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
        frameGenMultiplier = 4;
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
            parseMultiplierValue(value, config.frameGenMultiplier);
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

    if (key == keys::kFrameGenOutputFps) {
        config.frameGenOutputFps =
            parseIntValue(value, config.frameGenOutputFps);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kTrailFreeGuard) {
        config.trailFreeGuard = parseBoolValue(value, config.trailFreeGuard);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kExtraPresentedFrames) {
        config.extraPresentedFrames =
            parseBoolValue(value, config.extraPresentedFrames);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kHudOptimizerMode) {
        config.hudOptimizerMode =
            parseHudOptimizerModeValue(value, config.hudOptimizerMode);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kHudLatencyFlush) {
        config.hudLatencyFlush = parseBoolValue(value, config.hudLatencyFlush);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kHudStaticScreenBoost) {
        config.hudStaticScreenBoost =
            parseBoolValue(value, config.hudStaticScreenBoost);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kClearScreenOverlays) {
        config.clearScreenOverlays =
            parseBoolValue(value, config.clearScreenOverlays);
        if (config.clearScreenOverlays) {
            config.showHud = false;
        }
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

    return ConfigApplyResult::Unchanged;
}

void appendV1ModuleConfigs(pl::modmenu::ModuleBuilder &builder,
                           const FrameGenConfig &config) {
    builder
        .config(keys::kPreset,
                "Preset (0=PowerSaver, 1=Balanced, 2=Smooth, 3=Ultra, 4=Custom)",
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(static_cast<int>(config.preset)), "0", "4")
        .config(keys::kFrameGenMultiplier,
                "Rendered Frames Multiplier (2x / 3x / 4x Max)",
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(config.frameGenMultiplier), "2", "4")
        .config(keys::kFrameGenMode,
                "FrameGen Mode (0=Off, 1=Single-Swap Synthesis, 2=Multi-Swap)",
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(static_cast<int>(config.frameGenMode)), "0", "2")
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
                "FPS Cap with Frame Gen OFF (0=Unlimited)",
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(config.targetFps), "0", "240")
        .config(keys::kFrameGenOutputFps,
                "Frame Generation Output FPS Cap (0=Unlimited)",
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(config.frameGenOutputFps), "0", "240")
        .config(keys::kExtraPresentedFrames,
                "Present Generated Frames (REAL -> GEN -> REAL)",
                pl::modmenu::ConfigType::Toggle,
                config.extraPresentedFrames ? "true" : "false")
        .config(keys::kTrailFreeGuard,
                "Trail-Free Motion & Ghost Guard",
                pl::modmenu::ConfigType::Toggle,
                config.trailFreeGuard ? "true" : "false")
        .config(keys::kHudOptimizerMode,
                "HUD Optimizer (0=Off, 1=Balanced, 2=Ultra Low-Latency)",
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(static_cast<int>(config.hudOptimizerMode)), "0",
                "2")
        .config(keys::kHudLatencyFlush,
                "HUD Optimizer: Low-Latency GPU Flush",
                pl::modmenu::ConfigType::Toggle,
                config.hudLatencyFlush ? "true" : "false")
        .config(keys::kHudStaticScreenBoost,
                "HUD Optimizer: Inventory & UI Screen Boost",
                pl::modmenu::ConfigType::Toggle,
                config.hudStaticScreenBoost ? "true" : "false")
        .config(keys::kClearScreenOverlays,
                "HUD Optimizer: Remove All Screen Overlays",
                pl::modmenu::ConfigType::Toggle,
                config.clearScreenOverlays ? "true" : "false")
        .config(keys::kShowHud,
                "Show Optional Telemetry Badge",
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

    char fpsInfoBuf[288]{};
    if (telemetry.frameCapActive) {
        std::snprintf(fpsInfoBuf, sizeof(fpsInfoBuf),
                      "Presented: %.1f / %d FPS CAP (held %.0f%%) | Real: %.1f FPS "
                      "(target %d) | Generated: %.1f FPS (%dx)%s | 1%% Low: %.1f",
                      static_cast<double>(telemetry.presentedFps),
                      telemetry.presentedTargetFps,
                      static_cast<double>(telemetry.capHoldPercent),
                      static_cast<double>(telemetry.realFps),
                      telemetry.realTargetFps,
                      static_cast<double>(telemetry.generatedFps),
                      telemetry.activeMultiplier,
                      telemetry.embeddedSynthesis ? " [embedded]" : "",
                      static_cast<double>(telemetry.onePercentLowFps));
    } else {
        std::snprintf(fpsInfoBuf, sizeof(fpsInfoBuf),
                      "Presented: %.1f FPS (uncapped) | Real: %.1f FPS | "
                      "Generated: %.1f FPS (%dx)%s | 1%% Low: %.1f",
                      static_cast<double>(telemetry.presentedFps),
                      static_cast<double>(telemetry.realFps),
                      static_cast<double>(telemetry.generatedFps),
                      telemetry.activeMultiplier,
                      telemetry.embeddedSynthesis ? " [embedded]" : "",
                      static_cast<double>(telemetry.onePercentLowFps));
    }

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
    const std::vector<ConfigConditionV2> camEnabledCond = {
        {keys::kCameraSmoothingMode, ConfigConditionOpV2::NotEquals, "0"}
    };
    const std::vector<ConfigConditionV2> hudOptEnabledCond = {
        {keys::kHudOptimizerMode, ConfigConditionOpV2::NotEquals, "0"}
    };
    const std::vector<ConfigConditionV2> hudEnabledCond = {
        {keys::kShowHud, ConfigConditionOpV2::Truthy, "true"}
    };

    ConfigSchemaBuilder schema;
    schema.defaultCategory("general")
        .category("general", "Presets & Multiplier",
                  "Quick profiles, 2x/3x/4x frame multiplier, and live telemetry.")
        .category("framegen", "Frame Generation",
                  "Multi-rate sub-frame synthesis and FPS multiplication.")
        .category("camera", "Camera Smoothing",
                  "Sub-frame camera smoothing with 100% angle conservation.")
        .category("pacing", "HUD Optimizer & Pacing",
                  "HUD latency optimizer, UI screen boost, and frame pacing.");

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
            {"0", "Power Saver", "Minimal GPU overhead, 2x synthesis", "", false, "", false},
            {"1", "Balanced", "Recommended 2x synthesis + Smart Camera", "", false, "", false},
            {"2", "Smooth", "3x high-strength synthesis + silky camera filter", "", false, "", false},
            {"3", "Ultra", "4x multi-swap interpolation + ultra-fast response", "", false, "", false},
            {"4", "Custom", "User-customized settings", "", false, "", false},
        };
        schema.node(std::move(presetNode));
    }

    {
        ConfigNodeV2 multNode{};
        multNode.id = keys::kFrameGenMultiplier;
        multNode.key = keys::kFrameGenMultiplier;
        multNode.category = "general";
        multNode.section = "Frame Multiplier (2× / 3× / 4× Max)";
        multNode.title = "Rendered Frame Multiplier";
        multNode.description =
            "Choose how many frames are generated per real game frame "
            "(2× Double, 3× Triple, or 4× Quad Max).";
        multNode.type = ConfigControlTypeV2::Choice;
        multNode.choiceStyle = ConfigChoiceStyleV2::Segmented;
        multNode.defaultValue = "2";
        multNode.currentValue = std::to_string(config.frameGenMultiplier);
        multNode.hasCurrentValue = true;
        multNode.visibleWhen = fgEnabledCond;
        multNode.enabledWhen = fgEnabledCond;
        multNode.options = {
            {"2", "2×", "Double FPS (1 generated frame per real frame)", "", false, "", false},
            {"3", "3×", "Triple FPS (2 generated frames per real frame)", "", false, "", false},
            {"4", "4×", "Quad FPS — Max (3 generated frames per real frame)", "", false, "", false},
        };
        schema.node(std::move(multNode));
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
    addToggle(keys::kTrailFreeGuard, "framegen", "Artifact Protection",
              "Trail-Free Motion & Ghost Guard",
              "Detects unreliable mid-frame warps and keeps the razor-sharp real "
              "pixel instead, so fast camera turns never smear into motion trails.",
              config.trailFreeGuard, true, fgEnabledCond, fgEnabledCond);

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

    // --- Category: pacing & HUD Optimizer ---
    {
        ConfigNodeV2 hudOptNode{};
        hudOptNode.id = keys::kHudOptimizerMode;
        hudOptNode.key = keys::kHudOptimizerMode;
        hudOptNode.category = "pacing";
        hudOptNode.section = "HUD Optimizer";
        hudOptNode.title = "HUD Optimizer Mode";
        hudOptNode.description =
            "Eliminates HUD/crosshair input lag, skips redundant UI canvas "
            "redraws, and boosts inventory/chest screen responsiveness.";
        hudOptNode.type = ConfigControlTypeV2::Choice;
        hudOptNode.choiceStyle = ConfigChoiceStyleV2::Segmented;
        hudOptNode.defaultValue = "2";
        hudOptNode.currentValue =
            std::to_string(static_cast<int>(config.hudOptimizerMode));
        hudOptNode.hasCurrentValue = true;
        hudOptNode.options = {
            {"0", "Off", "Standard HUD rendering", "", false, "", false},
            {"1", "Balanced", "Flushes HUD latency during active movement", "", false, "", false},
            {"2", "Ultra Low-Latency", "Maximum HUD & inventory responsiveness (Recommended)", "", false, "", false},
        };
        schema.node(std::move(hudOptNode));
    }

    addToggle(keys::kHudLatencyFlush, "pacing", "HUD Optimizer",
              "HUD Pre-Swap Latency Flush",
              "Flushes GPU command queue before swap so crosshair and hotbar "
              "respond with zero buffered frame delay.",
              config.hudLatencyFlush, true, hudOptEnabledCond,
              hudOptEnabledCond);
    addToggle(keys::kHudStaticScreenBoost, "pacing", "HUD Optimizer",
              "Inventory & GUI Fast-Response Boost",
              "Optimizes frame pacing when inventory, chest, or pause menus "
              "are open.",
              config.hudStaticScreenBoost, true, hudOptEnabledCond,
              hudOptEnabledCond);
    addToggle(keys::kClearScreenOverlays, "pacing", "HUD Optimizer",
              "Remove Center-Screen Overlays & Logos",
              "Keeps the screen 100% clean with zero floating logos or boxes.",
              config.clearScreenOverlays, true, {}, {});

    addToggle(keys::kAutoTune, "pacing", "Frame Pacing",
              "Smart Auto-Tune Budget Guard",
              "Adapts synthesis workload if GPU frame times spike.",
              config.autoTune, true, {}, {});
    addSliderInt(keys::kTargetFps, "pacing", "Frame Pacing",
                 "FPS Cap with Frame Generation OFF (0 = Unlimited)",
                 "Applies to the final presented FPS when frame generation is "
                 "disabled, and as an upper bound when it is enabled.",
                 config.targetFps, 0, 0, 240, 5, "FPS", {}, {});
    addToggle(keys::kExtraPresentedFrames, "pacing", "Frame Pacing",
              "Present Generated Frames (REAL \u2192 GEN \u2192 REAL)",
              "Presents each synthesized frame as its own frame between two real "
              "frames, so generated frames fill the gaps instead of being blended "
              "on top of a real frame.",
              config.extraPresentedFrames, true, fgEnabledCond, fgEnabledCond);
    addSliderInt(keys::kFrameGenOutputFps, "pacing", "Frame Pacing",
                 "Frame Generation Output FPS (0 = Use FPS Cap)",
                 "Total presented FPS cap while frame generation is ON. It covers "
                 "real AND generated frames: 90 FPS = ~45 real + ~45 generated. "
                 "The tightest configured cap always wins, so frame generation can "
                 "never exceed the FPS cap above. 0 = follow the FPS cap above.",
                 config.frameGenOutputFps, 90, 0, 240, 5, "FPS", {}, {});
    addToggle(keys::kShowHud, "pacing", "Optional Telemetry Badge",
              "Show Telemetry Badge (Top-Left)",
              "Off by default so the screen stays completely clean.",
              config.showHud, false, {}, {});

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

    int fileSchemaVersion = 1;
    getInt("schema_version", fileSchemaVersion);

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
    getBool(keys::kTrailFreeGuard, outConfig.trailFreeGuard);
    getBool(keys::kExtraPresentedFrames, outConfig.extraPresentedFrames);

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
    getInt(keys::kFrameGenOutputFps, outConfig.frameGenOutputFps);

    int hudOptInt = static_cast<int>(outConfig.hudOptimizerMode);
    getInt(keys::kHudOptimizerMode, hudOptInt);
    outConfig.hudOptimizerMode = static_cast<HudOptimizerMode>(hudOptInt);
    getBool(keys::kHudLatencyFlush, outConfig.hudLatencyFlush);
    getBool(keys::kHudStaticScreenBoost, outConfig.hudStaticScreenBoost);
    getBool(keys::kClearScreenOverlays, outConfig.clearScreenOverlays);

    getBool(keys::kShowHud, outConfig.showHud);
    int hudStyleInt = static_cast<int>(outConfig.hudStyle);
    getInt(keys::kHudStyle, hudStyleInt);
    outConfig.hudStyle = static_cast<HudStyle>(hudStyleInt);

    getInt(keys::kHudOpacity, outConfig.hudOpacity);
    getFloat(keys::kHudPosX, outConfig.hudPosX);
    getFloat(keys::kHudPosY, outConfig.hudPosY);

    // Automatically migrate pre-v3 configs to clear any old on-screen HUD/logo
    if (fileSchemaVersion < 3) {
        outConfig.showHud = false;
        outConfig.clearScreenOverlays = true;
        outConfig.hudPosX = 16.0f;
        outConfig.hudPosY = 16.0f;
    }

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
        << "  \"" << keys::kCameraSmoothingMode << "\": " << static_cast<int>(c.cameraSmoothingMode) << ",\n"
        << "  \"" << keys::kCameraSmoothness << "\": " << formatFloat(c.cameraSmoothness, 4) << ",\n"
        << "  \"" << keys::kCameraResponsiveness << "\": " << formatFloat(c.cameraResponsiveness, 4) << ",\n"
        << "  \"" << keys::kMicroDeadzone << "\": " << formatFloat(c.microDeadzone, 4) << ",\n"
        << "  \"" << keys::kSpikeFilterDeg << "\": " << formatFloat(c.spikeFilterDeg, 2) << ",\n"
        << "  \"" << keys::kFovScaling << "\": " << (c.fovScaling ? "true" : "false") << ",\n"
        << "  \"" << keys::kAutoTune << "\": " << (c.autoTune ? "true" : "false") << ",\n"
        << "  \"" << keys::kTargetFps << "\": " << c.targetFps << ",\n"
        << "  \"" << keys::kSceneCutThreshold << "\": " << formatFloat(c.sceneCutThreshold, 2) << ",\n"
        << "  \"" << keys::kTrailFreeGuard << "\": " << (c.trailFreeGuard ? "true" : "false") << ",\n"
        << "  \"" << keys::kExtraPresentedFrames << "\": " << (c.extraPresentedFrames ? "true" : "false") << ",\n"
        << "  \"" << keys::kFrameGenOutputFps << "\": " << c.frameGenOutputFps << ",\n"
        << "  \"" << keys::kHudOptimizerMode << "\": " << static_cast<int>(c.hudOptimizerMode) << ",\n"
        << "  \"" << keys::kHudLatencyFlush << "\": " << (c.hudLatencyFlush ? "true" : "false") << ",\n"
        << "  \"" << keys::kHudStaticScreenBoost << "\": " << (c.hudStaticScreenBoost ? "true" : "false") << ",\n"
        << "  \"" << keys::kClearScreenOverlays << "\": " << (c.clearScreenOverlays ? "true" : "false") << ",\n"
        << "  \"" << keys::kShowHud << "\": " << (c.showHud ? "true" : "false") << ",\n"
        << "  \"" << keys::kHudStyle << "\": " << static_cast<int>(c.hudStyle) << ",\n"
        << "  \"" << keys::kHudOpacity << "\": " << c.hudOpacity << ",\n"
        << "  \"" << keys::kHudPosX << "\": " << formatFloat(c.hudPosX, 2) << ",\n"
        << "  \"" << keys::kHudPosY << "\": " << formatFloat(c.hudPosY, 2) << "\n"
        << "}\n";

    return out.good();
}

} // namespace framegen
