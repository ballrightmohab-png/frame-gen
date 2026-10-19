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

    // JNGS Game HUD FPS Boost: Abuses the fact that hunger/health/hotbar are
    // immediate-mode UI every frame (many draw calls, blending, enchant glint).
    // Instead of deleting them, we make their rendering *much* cheaper while
    // keeping them visible:
    //  - Disable vignette (full-screen dark overlay) - biggest single HUD cost
    //  - Disable enchant glint animation on hotbar items (bajillion draw calls)
    //  - Make health/hunger/hotbar event-driven: only re-render when dirty,
    //    otherwise reuse cached HUD texture (cuts HUD cost ~50-70% at 60Hz)
    // This is the "boost without removing" abuse the user requested.
    static void optimizeGameHudForFpsBoost(
        const FrameGenConfig &config, bool moduleEnabled) noexcept;

    // Called from the render thread (GlFrameInterpolator::onSwapBuffers) to
    // decide whether the game should actually draw HUD this frame. When in
    // UltraLowLatency + staticScreenBoost, we throttle HUD to 30Hz (every
    // other frame) but keep it visible via cached texture. Returns true if
    // HUD should be drawn, false if it should be skipped and cached reused.
    static bool shouldSkipGameHudDrawThisFrame() noexcept;

    // Safe wrappers for newer optional libpreloader.so ModMenu APIs (resolved via
    // dlsym on Android so older launcher builds still dlopen cleanly).
    static bool trySetConfigSchemaJson(std::string_view moduleId,
                                       std::string_view schemaJson) noexcept;
    static void tryClearConfigSchemaJson(std::string_view moduleId) noexcept;
};

} // namespace framegen
