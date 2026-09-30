#pragma once

#include "FrameGenConfig.hpp"

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

    // Safe wrappers for newer optional libpreloader.so ModMenu APIs (resolved via
    // dlsym on Android so older launcher builds still dlopen cleanly).
    static bool trySetConfigSchemaJson(std::string_view moduleId,
                                       std::string_view schemaJson) noexcept;
    static void tryClearConfigSchemaJson(std::string_view moduleId) noexcept;
};

} // namespace framegen
