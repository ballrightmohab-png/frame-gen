#include "HudRenderer.hpp"

#include <algorithm>
#include <cstdio>

#include <dlfcn.h>

namespace framegen {
namespace {

using SetConfigSchemaJsonFn = bool (*)(std::string_view, std::string_view);
using ClearConfigSchemaFn = void (*)(std::string_view);
using SubmitHudEditorElementsFn =
    void (*)(std::string_view,
             std::span<const pl::modmenu::HudEditorElement>);

void cardDimensionsForStyle(HudStyle style, float &outW,
                            float &outH) noexcept {
    switch (style) {
    case HudStyle::Minimal:
        outW = 148.0f;
        outH = 26.0f;
        break;
    case HudStyle::Compact:
        outW = 224.0f;
        outH = 48.0f;
        break;
    case HudStyle::Detailed:
        outW = 262.0f;
        outH = 84.0f;
        break;
    }
}

const char *shortCameraModeLabel(CameraSmoothingMode mode) noexcept {
    switch (mode) {
    case CameraSmoothingMode::Off:
        return "Off";
    case CameraSmoothingMode::Exponential:
        return "EMA";
    case CameraSmoothingMode::CriticallyDampedSpring:
        return "Spring";
    case CameraSmoothingMode::AdaptiveSmart:
        return "Smart";
    }
    return "Smart";
}

void trySubmitHudEditorElementsDynamic(
    std::string_view moduleId,
    std::span<const pl::modmenu::HudEditorElement> elements) noexcept {
#if defined(__ANDROID__)
    static auto fn = reinterpret_cast<SubmitHudEditorElementsFn>(dlsym(
        RTLD_DEFAULT,
        "_ZN2pl7modmenu23submitHudEditorElementsENSt6__ndk117basic_string_"
        "viewIcNS1_11char_traitsIcEEEENS1_4spanIKNS0_16HudEditorElementELm18"
        "446744073709551615EEE"));
    if (fn != nullptr) {
        fn(moduleId, elements);
    }
#else
    pl::modmenu::submitHudEditorElements(moduleId, elements);
#endif
}

} // namespace

std::vector<pl::modmenu::DrawCommand> HudRenderer::buildDrawCommands(
    const FrameGenConfig &config, const RuntimeTelemetrySnapshot &telemetry) {
    std::vector<pl::modmenu::DrawCommand> cmds;
    if (!telemetry.moduleEnabled || !config.showHud) {
        return cmds;
    }

    float cardW = 262.0f;
    float cardH = 84.0f;
    cardDimensionsForStyle(config.hudStyle, cardW, cardH);

    const float x = std::clamp(config.hudPosX, 0.0f, 4096.0f);
    const float y = std::clamp(config.hudPosY, 0.0f, 4096.0f);

    const std::uint32_t bgAlpha = static_cast<std::uint32_t>(
        std::clamp((config.hudOpacity * 255) / 100, 40, 245));
    const std::uint32_t bgColor = (bgAlpha << 24U) | 0x0010161EU;
    const std::uint32_t borderColor = 0x5538E8B0U;
    const std::uint32_t accentColor =
        telemetry.autoTuneReduced ? 0xFFF5B041U : 0xFF38E8B0U;
    const std::uint32_t titleColor = 0xFF58F2C2U;
    const std::uint32_t primaryTextColor = 0xFFF2F6FAU;
    const std::uint32_t secondaryTextColor = 0xFFB8C7D8U;

    cmds.reserve(8);

    // 1. Card background fill
    {
        pl::modmenu::DrawCommand bg{};
        bg.type = pl::modmenu::DrawCommandType::RectFilled;
        bg.x = x;
        bg.y = y;
        bg.w = cardW;
        bg.h = cardH;
        bg.color = bgColor;
        cmds.push_back(std::move(bg));
    }

    // 2. Left status accent bar
    {
        pl::modmenu::DrawCommand bar{};
        bar.type = pl::modmenu::DrawCommandType::RectFilled;
        bar.x = x;
        bar.y = y;
        bar.w = 4.0f;
        bar.h = cardH;
        bar.color = accentColor;
        cmds.push_back(std::move(bar));
    }

    // 3. Outer border
    {
        pl::modmenu::DrawCommand border{};
        border.type = pl::modmenu::DrawCommandType::Rect;
        border.x = x;
        border.y = y;
        border.w = cardW;
        border.h = cardH;
        border.color = borderColor;
        border.size = 1.0f;
        cmds.push_back(std::move(border));
    }

    auto addText = [&](float tx, float ty, std::string text,
                       std::uint32_t color, float size = 12.0f) {
        pl::modmenu::DrawCommand cmd{};
        cmd.type = pl::modmenu::DrawCommandType::Text;
        cmd.x = tx;
        cmd.y = ty;
        cmd.text = std::move(text);
        cmd.color = color;
        cmd.size = size;
        cmds.push_back(std::move(cmd));
    };

    const float textX = x + 10.0f;
    char lineBuf[128]{};

    if (config.hudStyle == HudStyle::Minimal) {
        std::snprintf(lineBuf, sizeof(lineBuf), "FG %.0f FPS (%.0fx%d)",
                      static_cast<double>(telemetry.effectiveFps),
                      static_cast<double>(telemetry.realFps),
                      telemetry.activeMultiplier);
        addText(textX, y + 6.0f, lineBuf, titleColor, 12.0f);
        return cmds;
    }

    if (config.hudStyle == HudStyle::Compact) {
        std::snprintf(lineBuf, sizeof(lineBuf), "LeviFrameGen [%s]",
                      presetName(config.preset));
        addText(textX, y + 6.0f, lineBuf, titleColor, 12.5f);

        std::snprintf(lineBuf, sizeof(lineBuf),
                      "FPS: %.0f -> %.0f | Jit: %.1fms | Cam: %s",
                      static_cast<double>(telemetry.realFps),
                      static_cast<double>(telemetry.effectiveFps),
                      static_cast<double>(telemetry.jitterMs),
                      shortCameraModeLabel(config.cameraSmoothingMode));
        addText(textX, y + 25.0f, lineBuf, primaryTextColor, 11.5f);
        return cmds;
    }

    // Detailed 4-line layout
    std::snprintf(lineBuf, sizeof(lineBuf), "LeviFrameGen v%s [%s]",
                  kModVersion, presetName(config.preset));
    addText(textX, y + 6.0f, lineBuf, titleColor, 12.5f);

    std::snprintf(lineBuf, sizeof(lineBuf),
                  "FPS: %.1f -> %.1f (%dx) | 1%% Low: %.0f",
                  static_cast<double>(telemetry.realFps),
                  static_cast<double>(telemetry.effectiveFps),
                  telemetry.activeMultiplier,
                  static_cast<double>(telemetry.onePercentLowFps));
    addText(textX, y + 24.0f, lineBuf, primaryTextColor, 11.5f);

    std::snprintf(lineBuf, sizeof(lineBuf),
                  "Frame: %.2f ms | Jitter: %.2f ms%s",
                  static_cast<double>(telemetry.frameTimeMs),
                  static_cast<double>(telemetry.jitterMs),
                  telemetry.autoTuneReduced ? " [AutoTune]" : "");
    addText(textX, y + 42.0f, lineBuf, secondaryTextColor, 11.5f);

    std::snprintf(lineBuf, sizeof(lineBuf), "Cam: %s | FOV: %.0f deg | %s",
                  shortCameraModeLabel(config.cameraSmoothingMode),
                  static_cast<double>(telemetry.currentFovDeg),
                  telemetry.turnDeltaHooked ? "Native Hook" : "Optical Sync");
    addText(textX, y + 60.0f, lineBuf, secondaryTextColor, 11.5f);

    return cmds;
}

pl::modmenu::HudEditorElement HudRenderer::buildHudEditorElement(
    const FrameGenConfig &config) {
    float cardW = 262.0f;
    float cardH = 84.0f;
    cardDimensionsForStyle(config.hudStyle, cardW, cardH);

    pl::modmenu::HudEditorElement elem{};
    elem.elementId = kHudElementId;
    elem.displayName = "LeviFrameGen Telemetry";
    elem.x = config.hudPosX;
    elem.y = config.hudPosY;
    elem.width = cardW;
    elem.height = cardH;
    elem.positionKeyX = keys::kHudPosX;
    elem.positionKeyY = keys::kHudPosY;
    elem.snapFlags = pl::modmenu::HudSnapFlags::HudSnapElements |
                     pl::modmenu::HudSnapFlags::HudSnapGrid |
                     pl::modmenu::HudSnapFlags::HudSnapScreenCenter;
    elem.gridSize = 8.0f;
    elem.gridGap = 4.0f;
    elem.snapThreshold = 8.0f;
    elem.snapGroup = "hud";
    return elem;
}

void HudRenderer::publishHud(
    const FrameGenConfig &config,
    const RuntimeTelemetrySnapshot &telemetry) noexcept {
    if (!telemetry.moduleEnabled || !config.showHud ||
        config.clearScreenOverlays) {
        clearHud();
        return;
    }
    const auto cmds = buildDrawCommands(config, telemetry);
    pl::modmenu::submitDrawCommands(kModuleId, cmds);

    const pl::modmenu::HudEditorElement elem = buildHudEditorElement(config);
    const pl::modmenu::HudEditorElement elems[1] = {elem};
    trySubmitHudEditorElementsDynamic(kModuleId, elems);
}

void HudRenderer::clearHud() noexcept {
    const std::vector<pl::modmenu::DrawCommand> emptyCmds;
    pl::modmenu::submitDrawCommands(kModuleId, emptyCmds);
    const std::vector<pl::modmenu::HudEditorElement> emptyElems;
    trySubmitHudEditorElementsDynamic(kModuleId, emptyElems);
#if defined(__ANDROID__)
    using UnregisterButtonFn = void (*)(std::string_view);
    static auto unregFn = reinterpret_cast<UnregisterButtonFn>(dlsym(
        RTLD_DEFAULT,
        "_ZN2pl7modmenu16unregisterButtonENSt6__ndk117basic_string_viewIcNS1_"
        "11char_traitsIcEEEE"));
    if (unregFn != nullptr) {
        unregFn("leviframegen.quick_button");
    }
#else
    pl::modmenu::unregisterButton("leviframegen.quick_button");
#endif
}

bool HudRenderer::trySetConfigSchemaJson(std::string_view moduleId,
                                         std::string_view schemaJson) noexcept {
#if defined(__ANDROID__)
    static auto fn = reinterpret_cast<SetConfigSchemaJsonFn>(dlsym(
        RTLD_DEFAULT,
        "_ZN2pl7modmenu19setConfigSchemaJsonENSt6__ndk117basic_string_viewIc"
        "NS1_11char_traitsIcEEEES5_"));
    if (fn != nullptr) {
        return fn(moduleId, schemaJson);
    }
    return false;
#else
    return pl::modmenu::setConfigSchemaJson(moduleId, schemaJson);
#endif
}

void HudRenderer::tryClearConfigSchemaJson(std::string_view moduleId) noexcept {
#if defined(__ANDROID__)
    static auto fn = reinterpret_cast<ClearConfigSchemaFn>(dlsym(
        RTLD_DEFAULT,
        "_ZN2pl7modmenu17clearConfigSchemaENSt6__ndk117basic_string_viewIc"
        "NS1_11char_traitsIcEEEE"));
    if (fn != nullptr) {
        fn(moduleId);
    }
#else
    pl::modmenu::clearConfigSchema(moduleId);
#endif
}

} // namespace framegen
