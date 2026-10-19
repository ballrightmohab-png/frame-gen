#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <pl/ModMenu.hpp>

namespace framegen {

inline constexpr const char *kModId = "jngsframegen";
inline constexpr const char *kModuleId = "jngsframegen.module";
inline constexpr const char *kHudElementId = "jngsframegen.hud_card";
inline constexpr const char *kModName = "JNGS Frame Gen";
inline constexpr const char *kModVersion = "3.1.0";
inline constexpr const char *kModAuthor = "JNGS";

// Configuration keys exposed to LeviLauncher Mod Menu (V1 & V2)
namespace keys {
inline constexpr const char *kPreset = "preset";
inline constexpr const char *kFrameGenMode = "frame_gen_mode";
inline constexpr const char *kFrameGenMultiplier = "frame_gen_multiplier";
inline constexpr const char *kBlendStrength = "blend_strength";
inline constexpr const char *kMotionScale = "motion_scale";
inline constexpr const char *kHudProtection = "hud_protection";
inline constexpr const char *kSceneCutThreshold = "scene_cut_threshold";

inline constexpr const char *kCameraSmoothingMode = "camera_smoothing_mode";
inline constexpr const char *kCameraSmoothness = "camera_smoothness";
inline constexpr const char *kCameraResponsiveness = "camera_responsiveness";
inline constexpr const char *kMicroDeadzone = "micro_deadzone";
inline constexpr const char *kSpikeFilterDeg = "spike_filter_deg";
inline constexpr const char *kFovScaling = "fov_scaling";

inline constexpr const char *kAutoTune = "auto_tune";
inline constexpr const char *kTargetFps = "target_fps";

inline constexpr const char *kHudOptimizerMode = "hud_optimizer_mode";
inline constexpr const char *kHudLatencyFlush = "hud_latency_flush";
inline constexpr const char *kHudStaticScreenBoost = "hud_static_screen_boost";
inline constexpr const char *kClearScreenOverlays = "clear_screen_overlays";
inline constexpr const char *kEnableMotionBlur = "enable_motion_blur";
inline constexpr const char *kTurboMode = "turbo_mode";
inline constexpr const char *kReflexMode = "reflex_mode";
inline constexpr const char *kCrystalSharp = "crystal_sharp";
inline constexpr const char *kKillVignette = "kill_vignette";

inline constexpr const char *kShowHud = "show_hud";
inline constexpr const char *kHudStyle = "hud_style";
inline constexpr const char *kHudOpacity = "hud_opacity";
inline constexpr const char *kHudPosX = "hud_pos_x";
inline constexpr const char *kHudPosY = "hud_pos_y";

// Action button keys in V2 RuntimeConfigView
inline constexpr const char *kActionResetDefaults = "action_reset_defaults";
inline constexpr const char *kActionResetTemporal = "action_reset_temporal";
inline constexpr const char *kActionCyclePreset = "action_cycle_preset";
} // namespace keys

enum class QualityPreset : int {
    PowerSaver = 0,
    Balanced = 1,
    Smooth = 2,
    Ultra = 3,
    Custom = 4
};

enum class FrameGenMode : int {
    Off = 0,
    SingleSwapSynthesis = 1,
    MultiSwapInterpolation = 2
};

enum class CameraSmoothingMode : int {
    Off = 0,
    Exponential = 1,
    CriticallyDampedSpring = 2,
    AdaptiveSmart = 3
};

enum class HudStyle : int {
    Compact = 0,
    Detailed = 1,
    Minimal = 2
};

enum class HudOptimizerMode : int {
    Off = 0,
    Balanced = 1,
    UltraLowLatency = 2
};

struct RuntimeTelemetrySnapshot {
    bool moduleEnabled = true;
    bool eglHooked = false;
    bool presentationTimeSupported = false;
    bool turnDeltaHooked = false;
    bool fovHooked = false;
    bool perspectiveHooked = false;

    float realFps = 0.0f;
    float effectiveFps = 0.0f;
    float onePercentLowFps = 0.0f;
    float frameTimeMs = 0.0f;
    float jitterMs = 0.0f;

    float lastMotionU = 0.0f;
    float lastMotionV = 0.0f;
    float cameraPitchVelDeg = 0.0f;
    float cameraYawVelDeg = 0.0f;
    float currentFovDeg = 70.0f;
    int cameraPerspective = 0;

    std::uint64_t totalRealFrames = 0;
    std::uint64_t totalSynthesizedFrames = 0;
    std::uint64_t sceneCutResets = 0;
    int activeMultiplier = 1;
    bool autoTuneReduced = false;
};

struct FrameGenConfig {
    int schemaVersion = 3;

    QualityPreset preset = QualityPreset::Balanced;

    // Frame Generation settings
    FrameGenMode frameGenMode = FrameGenMode::SingleSwapSynthesis;
    int frameGenMultiplier = 2;          // 2x .. 4x
    float blendStrength = 0.65f;         // 0.0 .. 1.0
    float motionScale = 1.0f;            // 0.0 .. 2.0
    float hudProtection = 0.88f;         // 0.0 .. 1.0
    float sceneCutThreshold = 42.0f;     // 10.0 .. 120.0 deg/frame

    // Camera Smoothing settings
    CameraSmoothingMode cameraSmoothingMode = CameraSmoothingMode::AdaptiveSmart;
    float cameraSmoothness = 0.55f;      // 0.0 .. 0.95
    float cameraResponsiveness = 1.05f;  // 0.1 .. 2.5
    float microDeadzone = 0.025f;        // 0.0 .. 0.5 deg
    float spikeFilterDeg = 75.0f;        // 10.0 .. 180.0 deg
    bool fovScaling = true;

    // Frame Pacing & Auto-Tune
    bool autoTune = true;
    int targetFps = 0;                   // 0 = Unlimited, or 30 .. 240

    // HUD Optimizer settings
    HudOptimizerMode hudOptimizerMode = HudOptimizerMode::UltraLowLatency;
    bool hudLatencyFlush = true;
    bool hudStaticScreenBoost = true;
    bool clearScreenOverlays = true;

    // Motion Blur toggle — requested to keep as setting (ON = blur, OFF = sharp, default OFF)
    bool enableMotionBlur = false;

    // Cool toggles - actually do something
    bool turboMode = false;        // Turbo 4x burst - forces 4x multi-swap with max blend
    bool reflexMode = false;       // Reflex low latency - bypass pacer, instant present
    bool crystalSharp = true;      // Crystal Sharp - extra sharpness, no blending ghost
    bool killVignette = true;      // Vignette Killer - kills dark overlay for FPS

    // Optional Telemetry HUD (OFF by default so screen stays 100% clean)
    bool showHud = false;
    HudStyle hudStyle = HudStyle::Minimal;
    int hudOpacity = 75;                 // 20 .. 100
    float hudPosX = 16.0f;
    float hudPosY = 16.0f;

    void normalize() noexcept;
    void applyPreset(QualityPreset newPreset) noexcept;

    // BedrockTools-style JSON with ALL toggles (for ConfigManager Modules["JNGS Frame Gen"])
    nlohmann::json toJson() const;
    void fromJson(const nlohmann::json& j);
};

const char *presetName(QualityPreset preset) noexcept;
const char *frameGenModeName(FrameGenMode mode) noexcept;
const char *cameraSmoothingModeName(CameraSmoothingMode mode) noexcept;
const char *hudStyleName(HudStyle style) noexcept;
const char *hudOptimizerModeName(HudOptimizerMode mode) noexcept;

enum class ConfigApplyResult {
    Unchanged = 0,
    UpdatedConfig,
    TriggeredResetDefaults,
    TriggeredResetTemporal,
    TriggeredCyclePreset
};

// Applies a single key/value pair received from LeviLauncher's ModMenu UI.
ConfigApplyResult applyConfigKeyValue(FrameGenConfig &config,
                                      std::string_view key,
                                      std::string_view value) noexcept;

// Populates V1 ModuleBuilder config entries for backward compatibility with ModConfigView.
void appendV1ModuleConfigs(pl::modmenu::ModuleBuilder &builder,
                           const FrameGenConfig &config);

// Builds the V2 RuntimeConfigView JSON schema with sections, conditional rules,
// action buttons, and live telemetry info rows.
std::string buildV2ConfigSchemaJson(const FrameGenConfig &config,
                                    const RuntimeTelemetrySnapshot &telemetry);

// Persistence helpers (JSON file at <dataPath>/config.json)
bool loadConfigFromFile(const std::filesystem::path &path,
                        FrameGenConfig &outConfig);
bool saveConfigToFile(const std::filesystem::path &path,
                      const FrameGenConfig &config);

// BedrockTools ConfigManager integration - ALL toggles via Modules["JNGS Frame Gen"] with worker thread
bool saveConfigViaManager(const FrameGenConfig &config);
bool loadConfigViaManager(FrameGenConfig &outConfig);

} // namespace framegen
