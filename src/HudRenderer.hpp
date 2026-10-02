#pragma once

#include "FrameGenConfig.hpp"

#include <jni.h>
#include <span>
#include <string_view>
#include <vector>

#include <pl/ModMenu.hpp>

namespace framegen {

class HudRenderer {
public:
    // Builds the vector of DrawCommand primitives for the current config and telemetry.
    static std::vector<pl::modmenu::DrawCommand> buildDrawCommands(
        const FrameGenConfig &config,
        const RuntimeTelemetrySnapshot &telemetry);

    // Builds the HudEditorElement descriptor for LeviLauncher's HUD Editor.
    static pl::modmenu::HudEditorElement buildHudEditorElement(
        const FrameGenConfig &config);

    // Publishes draw commands and HUD editor bounds to LeviLauncher's ModMenu bridge.
    static void publishHud(const FrameGenConfig &config,
                           const RuntimeTelemetrySnapshot &telemetry) noexcept;

    // Clears all HUD draw commands and editor elements when disabled/unloaded.
    static void clearHud() noexcept;

    // Android JNI HUD Optimizer & Center-Screen Logo / Dimming Guard:
    // - Relocates any inbuilt overlays that LeviLaunchroid spawns at (centerX, centerY)
    //   out of the middle of the screen to clean edge coordinates.
    // - Clears Window FLAG_DIM_BEHIND and HudOverlay elevation/draw overhead so the
    //   screen never dims and HWUI does not composite redundant fullscreen layers.
    static void optimizeAndroidHudAndOverlays(
        JavaVM *vm, const FrameGenConfig &config, bool moduleEnabled) noexcept;

    // Safe wrappers for newer optional libpreloader.so ModMenu APIs (resolved via
    // dlsym on Android so older launcher builds still dlopen cleanly).
    static bool trySetConfigSchemaJson(std::string_view moduleId,
                                       std::string_view schemaJson) noexcept;
    static void tryClearConfigSchemaJson(std::string_view moduleId) noexcept;
};

} // namespace framegen
