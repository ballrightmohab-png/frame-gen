#pragma once

#include <string>
#include <string_view>

#include <pl/Config.hpp>

namespace framegen {

enum class RenderMode {
  Disabled,
  Simple,
  Debug,
};

struct FrameGenConfig {
  int version = 1;
  bool enabled = true;
  int targetFPS = 60;
  int opacity = 80;
  double scale = 1.0;
  RenderMode mode = RenderMode::Simple;
  std::string overlayColor = "#00FF00";
};

inline constexpr int kMinFPS = 30;
inline constexpr int kMaxFPS = 240;
inline constexpr int kMinOpacity = 0;
inline constexpr int kMaxOpacity = 100;
inline constexpr double kMinScale = 0.5;
inline constexpr double kMaxScale = 2.0;
inline constexpr std::string_view kModeMenuOptions = "Disabled,Simple,Debug";

} // namespace framegen

namespace pl::config {

template <> struct Schema<framegen::FrameGenConfig> {
  static constexpr std::string_view title = "LeviFrameGen Configuration";
  static constexpr std::string_view description =
      "Frame generation and rendering configuration for native Minecraft mods.";

  static constexpr FieldSchema field(std::string_view name) {
    if (name == "version") {
      return {"Version", "Config schema version managed by the mod.",
              std::nullopt, std::nullopt, true};
    }
    if (name == "enabled") {
      return {"Enabled",
              "Controls whether frame generation is active.",
              std::nullopt, std::nullopt, false};
    }
    if (name == "targetFPS") {
      return {"Target FPS", "Target frames per second for generation.",
              framegen::kMinFPS, framegen::kMaxFPS, false};
    }
    if (name == "opacity") {
      return {"Overlay Opacity", "Overlay opacity percentage.",
              framegen::kMinOpacity, framegen::kMaxOpacity, false};
    }
    if (name == "scale") {
      return {"Overlay Scale", "Overlay scale multiplier.", framegen::kMinScale,
              framegen::kMaxScale, false};
    }
    if (name == "mode") {
      return {"Render Mode", "Frame generation detail level.", std::nullopt,
              std::nullopt, false};
    }
    if (name == "overlayColor") {
      return {"Overlay Color",
              "Overlay color in #RRGGBB or #AARRGGBB form.",
              std::nullopt, std::nullopt, false};
    }
    return {};
  }
};

} // namespace pl::config
