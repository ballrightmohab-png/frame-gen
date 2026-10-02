#include "HudRenderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

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
        outH = 100.0f;
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

    float x = std::clamp(config.hudPosX, 0.0f, 4096.0f);
    float y = std::clamp(config.hudPosY, 0.0f, 4096.0f);

    // Never allow the HUD card to sit in the middle of the screen over the
    // crosshair (e.g. if LeviLaunchroid's resetAllPositionsToCenter was called).
    if (config.clearScreenOverlays && x > 180.0f && x < 1740.0f &&
        y > 100.0f && y < 980.0f) {
        x = 16.0f;
        y = 16.0f;
    }

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
        // Presented FPS is the real, verifiable output rate (real + generated).
        if (telemetry.frameCapActive) {
            std::snprintf(lineBuf, sizeof(lineBuf),
                          "Pres %.0f/%d | R %.0f + G %.0f (%dx)",
                          static_cast<double>(telemetry.presentedFps),
                          telemetry.presentedTargetFps,
                          static_cast<double>(telemetry.realFps),
                          static_cast<double>(telemetry.generatedFps),
                          telemetry.activeMultiplier);
        } else {
            std::snprintf(lineBuf, sizeof(lineBuf),
                          "Pres %.0f | R %.0f + G %.0f (%dx)",
                          static_cast<double>(telemetry.presentedFps),
                          static_cast<double>(telemetry.realFps),
                          static_cast<double>(telemetry.generatedFps),
                          telemetry.activeMultiplier);
        }
        addText(textX, y + 6.0f, lineBuf, titleColor, 12.0f);
        return cmds;
    }

    if (config.hudStyle == HudStyle::Compact) {
        std::snprintf(lineBuf, sizeof(lineBuf), "LeviFrameGen [%s]",
                      presetName(config.preset));
        addText(textX, y + 6.0f, lineBuf, titleColor, 12.5f);

        std::snprintf(lineBuf, sizeof(lineBuf),
                      "Real %.0f + Gen %.0f = %.0f FPS%s | Jit %.1fms",
                      static_cast<double>(telemetry.realFps),
                      static_cast<double>(telemetry.generatedFps),
                      static_cast<double>(telemetry.presentedFps),
                      telemetry.frameCapActive ? " (capped)" : "",
                      static_cast<double>(telemetry.jitterMs));
        addText(textX, y + 25.0f, lineBuf, primaryTextColor, 11.5f);
        return cmds;
    }

    // Detailed 4-line layout
    std::snprintf(lineBuf, sizeof(lineBuf), "LeviFrameGen v%s [%s]",
                  kModVersion, presetName(config.preset));
    addText(textX, y + 6.0f, lineBuf, titleColor, 12.5f);

    std::snprintf(lineBuf, sizeof(lineBuf),
                  "Real: %.1f | Generated: %.1f%s | Presented: %.1f FPS (%dx)",
                  static_cast<double>(telemetry.realFps),
                  static_cast<double>(telemetry.generatedFps),
                  telemetry.embeddedSynthesis ? "(emb)" : "",
                  static_cast<double>(telemetry.presentedFps),
                  telemetry.activeMultiplier);
    addText(textX, y + 24.0f, lineBuf, primaryTextColor, 11.5f);

    if (telemetry.frameCapActive) {
        std::snprintf(lineBuf, sizeof(lineBuf),
                      "Output cap: %d FPS (held %.0f%%) | Real target: %d FPS | 1%% Low: %.0f",
                      telemetry.presentedTargetFps,
                      static_cast<double>(telemetry.capHoldPercent),
                      telemetry.realTargetFps,
                      static_cast<double>(telemetry.onePercentLowFps));
    } else {
        std::snprintf(lineBuf, sizeof(lineBuf),
                      "Output cap: Unlimited | 1%% Low: %.0f FPS",
                      static_cast<double>(telemetry.onePercentLowFps));
    }
    addText(textX, y + 42.0f, lineBuf, secondaryTextColor, 11.5f);

    std::snprintf(lineBuf, sizeof(lineBuf),
                  "Frame: %.2f ms | Jitter: %.2f ms%s | Present: %.2f ms",
                  static_cast<double>(telemetry.frameTimeMs),
                  static_cast<double>(telemetry.jitterMs),
                  telemetry.autoTuneReduced ? " [AutoTune]" : "",
                  static_cast<double>(telemetry.presentedIntervalMs));
    addText(textX, y + 60.0f, lineBuf, secondaryTextColor, 11.5f);

    std::snprintf(lineBuf, sizeof(lineBuf), "Cam: %s | FOV: %.0f deg | %s",
                  shortCameraModeLabel(config.cameraSmoothingMode),
                  static_cast<double>(telemetry.currentFovDeg),
                  telemetry.turnDeltaHooked
                      ? "Native Hook"
                      : "Optical Sync");
    addText(textX, y + 78.0f, lineBuf, secondaryTextColor, 11.5f);

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
    if (!telemetry.moduleEnabled || !config.showHud) {
        clearHud();
        return;
    }
    const auto cmds = buildDrawCommands(config, telemetry);
    pl::modmenu::submitDrawCommands(kModuleId, cmds);

    // Do not submit HudEditorElements when clearScreenOverlays is active so
    // LeviLaunchroid never draws a green center-screen selection box.
    if (config.clearScreenOverlays) {
        const std::vector<pl::modmenu::HudEditorElement> emptyElems;
        trySubmitHudEditorElementsDynamic(kModuleId, emptyElems);
    } else {
        const pl::modmenu::HudEditorElement elem = buildHudEditorElement(config);
        const pl::modmenu::HudEditorElement elems[1] = {elem};
        trySubmitHudEditorElementsDynamic(kModuleId, elems);
    }
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

void HudRenderer::optimizeAndroidHudAndOverlays(
    JavaVM *vm, const FrameGenConfig &config, bool moduleEnabled) noexcept {
#if defined(__ANDROID__)
    if (vm == nullptr) {
        return;
    }

    JNIEnv *env = nullptr;
    bool attachedHere = false;
    const jint envState =
        vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (envState == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || env == nullptr) {
            return;
        }
        attachedHere = true;
    } else if (envState != JNI_OK || env == nullptr) {
        return;
    }

    auto clearJniException = [&]() noexcept -> bool {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return true;
        }
        return false;
    };

    static std::mutex sJniCacheMutex;
    static jclass sOverlayMgrClass = nullptr;
    {
        std::lock_guard<std::mutex> lock(sJniCacheMutex);
        if (sOverlayMgrClass == nullptr) {
            jclass localCls = env->FindClass(
                "org/levimc/launcher/core/mods/inbuilt/overlay/"
                "InbuiltOverlayManager");
            if (!clearJniException() && localCls != nullptr) {
                sOverlayMgrClass =
                    static_cast<jclass>(env->NewGlobalRef(localCls));
                env->DeleteLocalRef(localCls);
            }
        }
    }

    // 1. Pre-populate and sanitize SharedPreferences("inbuilt_mods_prefs")
    // so LeviLaunchroid's InbuiltOverlayManager NEVER spawns overlays in the
    // middle of the screen at (widthPixels/2 - 26dp, heightPixels/2 - 26dp).
    if (moduleEnabled &&
        (config.clearScreenOverlays ||
         config.hudOptimizerMode != HudOptimizerMode::Off)) {
        jclass atCls = env->FindClass("android/app/ActivityThread");
        if (!clearJniException() && atCls != nullptr) {
            jmethodID curAppMid = env->GetStaticMethodID(
                atCls, "currentApplication", "()Landroid/app/Application;");
            if (!clearJniException() && curAppMid != nullptr) {
                jobject app = env->CallStaticObjectMethod(atCls, curAppMid);
                if (!clearJniException() && app != nullptr) {
                    jclass ctxCls = env->GetObjectClass(app);
                    jmethodID getResMid = env->GetMethodID(
                        ctxCls, "getResources",
                        "()Landroid/content/res/Resources;");
                    jmethodID getPrefsMid = env->GetMethodID(
                        ctxCls, "getSharedPreferences",
                        "(Ljava/lang/String;I)Landroid/content/"
                        "SharedPreferences;");

                    int screenW = 1920;
                    int screenH = 1080;
                    float density = 2.75f;

                    if (!clearJniException() && getResMid != nullptr) {
                        jobject res = env->CallObjectMethod(app, getResMid);
                        if (!clearJniException() && res != nullptr) {
                            jclass resCls = env->GetObjectClass(res);
                            jmethodID getDmMid = env->GetMethodID(
                                resCls, "getDisplayMetrics",
                                "()Landroid/util/DisplayMetrics;");
                            if (!clearJniException() && getDmMid != nullptr) {
                                jobject dm =
                                    env->CallObjectMethod(res, getDmMid);
                                if (!clearJniException() && dm != nullptr) {
                                    jclass dmCls = env->GetObjectClass(dm);
                                    jfieldID wFid = env->GetFieldID(
                                        dmCls, "widthPixels", "I");
                                    jfieldID hFid = env->GetFieldID(
                                        dmCls, "heightPixels", "I");
                                    jfieldID dFid =
                                        env->GetFieldID(dmCls, "density", "F");
                                    if (!clearJniException() &&
                                        wFid != nullptr && hFid != nullptr &&
                                        dFid != nullptr) {
                                        const int rawW =
                                            env->GetIntField(dm, wFid);
                                        const int rawH =
                                            env->GetIntField(dm, hFid);
                                        const float rawD =
                                            env->GetFloatField(dm, dFid);
                                        if (rawW > 0 && rawH > 0) {
                                            screenW = std::max(rawW, rawH);
                                            screenH = std::min(rawW, rawH);
                                        }
                                        if (rawD > 0.5f && rawD < 10.0f) {
                                            density = rawD;
                                        }
                                    }
                                    env->DeleteLocalRef(dmCls);
                                    env->DeleteLocalRef(dm);
                                }
                            }
                            env->DeleteLocalRef(resCls);
                            env->DeleteLocalRef(res);
                        }
                    }

                    if (!clearJniException() && getPrefsMid != nullptr) {
                        jstring prefsName =
                            env->NewStringUTF("inbuilt_mods_prefs");
                        jobject prefs = env->CallObjectMethod(
                            app, getPrefsMid, prefsName, 0);
                        env->DeleteLocalRef(prefsName);

                        if (!clearJniException() && prefs != nullptr) {
                            jclass prefsCls = env->GetObjectClass(prefs);
                            jmethodID containsMid = env->GetMethodID(
                                prefsCls, "contains", "(Ljava/lang/String;)Z");
                            jmethodID getIntMid = env->GetMethodID(
                                prefsCls, "getInt", "(Ljava/lang/String;I)I");
                            jmethodID editMid = env->GetMethodID(
                                prefsCls, "edit",
                                "()Landroid/content/SharedPreferences$Editor;");

                            if (!clearJniException() && containsMid != nullptr &&
                                getIntMid != nullptr && editMid != nullptr) {
                                jobject editor =
                                    env->CallObjectMethod(prefs, editMid);
                                if (!clearJniException() && editor != nullptr) {
                                    jclass editorCls =
                                        env->GetObjectClass(editor);
                                    jmethodID putIntMid = env->GetMethodID(
                                        editorCls, "putInt",
                                        "(Ljava/lang/String;I)Landroid/content/"
                                        "SharedPreferences$Editor;");
                                    jmethodID putBoolMid = env->GetMethodID(
                                        editorCls, "putBoolean",
                                        "(Ljava/lang/String;Z)Landroid/content/"
                                        "SharedPreferences$Editor;");
                                    jmethodID applyMid = env->GetMethodID(
                                        editorCls, "apply", "()V");

                                    const int centerX =
                                        screenW / 2 -
                                        static_cast<int>(26.0f * density);
                                    const int centerY =
                                        screenH / 2 -
                                        static_cast<int>(26.0f * density);
                                    const int centerDeadzoneX =
                                        static_cast<int>(96.0f * density);
                                    const int centerDeadzoneY =
                                        static_cast<int>(96.0f * density);

                                    struct EdgeSlot {
                                        const char *modId;
                                        int edgeX;
                                        int edgeY;
                                    };
                                    const int leftX =
                                        static_cast<int>(14.0f * density);
                                    const int rightX = std::max(
                                        leftX,
                                        screenW -
                                            static_cast<int>(70.0f * density));
                                    const EdgeSlot slots[] = {
                                        {"fps_display", leftX,
                                         static_cast<int>(12.0f * density)},
                                        {"cps_display", leftX,
                                         static_cast<int>(42.0f * density)},
                                        {"quick_drop", leftX,
                                         static_cast<int>(210.0f * density)},
                                        {"camera_perspective", leftX,
                                         static_cast<int>(272.0f * density)},
                                        {"toggle_hud", leftX,
                                         static_cast<int>(334.0f * density)},
                                        {"auto_sprint", leftX,
                                         static_cast<int>(396.0f * density)},
                                        {"zoom", rightX,
                                         static_cast<int>(140.0f * density)},
                                        {"snaplook", rightX,
                                         static_cast<int>(202.0f * density)},
                                        {"virtual_cursor", rightX,
                                         static_cast<int>(264.0f * density)},
                                        {"gyro", rightX,
                                         static_cast<int>(326.0f * density)},
                                    };

                                    bool modified = false;
                                    if (!clearJniException() &&
                                        putIntMid != nullptr &&
                                        applyMid != nullptr) {
                                        for (const auto &slot : slots) {
                                            std::string keyX =
                                                std::string("overlay_pos_x_") +
                                                slot.modId;
                                            std::string keyY =
                                                std::string("overlay_pos_y_") +
                                                slot.modId;
                                            jstring jKeyX =
                                                env->NewStringUTF(keyX.c_str());
                                            jstring jKeyY =
                                                env->NewStringUTF(keyY.c_str());

                                            const jboolean hasX =
                                                env->CallBooleanMethod(
                                                    prefs, containsMid, jKeyX);
                                            const jboolean hasY =
                                                env->CallBooleanMethod(
                                                    prefs, containsMid, jKeyY);
                                            const int curX =
                                                env->CallIntMethod(
                                                    prefs, getIntMid, jKeyX,
                                                    centerX);
                                            const int curY =
                                                env->CallIntMethod(
                                                    prefs, getIntMid, jKeyY,
                                                    centerY);
                                            clearJniException();

                                            const bool inCenter =
                                                (std::abs(curX - centerX) <=
                                                 centerDeadzoneX) &&
                                                (std::abs(curY - centerY) <=
                                                 centerDeadzoneY);

                                            if (!hasX || !hasY || inCenter) {
                                                jobject r1 =
                                                    env->CallObjectMethod(
                                                        editor, putIntMid,
                                                        jKeyX, slot.edgeX);
                                                if (r1 != nullptr) {
                                                    env->DeleteLocalRef(r1);
                                                }
                                                jobject r2 =
                                                    env->CallObjectMethod(
                                                        editor, putIntMid,
                                                        jKeyY, slot.edgeY);
                                                if (r2 != nullptr) {
                                                    env->DeleteLocalRef(r2);
                                                }
                                                clearJniException();
                                                modified = true;
                                            }

                                            env->DeleteLocalRef(jKeyX);
                                            env->DeleteLocalRef(jKeyY);
                                        }

                                        if (config.clearScreenOverlays &&
                                            putBoolMid != nullptr) {
                                            jstring petKey = env->NewStringUTF(
                                                "inbuilt_mod_enabled_chick_pet");
                                            jobject rPet =
                                                env->CallObjectMethod(
                                                    editor, putBoolMid, petKey,
                                                    JNI_FALSE);
                                            if (rPet != nullptr) {
                                                env->DeleteLocalRef(rPet);
                                            }
                                            env->DeleteLocalRef(petKey);
                                            clearJniException();
                                            modified = true;
                                        }

                                        if (modified) {
                                            env->CallVoidMethod(editor,
                                                                applyMid);
                                            clearJniException();
                                        }
                                    }
                                    env->DeleteLocalRef(editorCls);
                                    env->DeleteLocalRef(editor);
                                }
                            }
                            env->DeleteLocalRef(prefsCls);
                            env->DeleteLocalRef(prefs);
                        }
                    }
                    env->DeleteLocalRef(ctxCls);
                    env->DeleteLocalRef(app);
                }
            }
            env->DeleteLocalRef(atCls);
        }
    }

    // 2. Runtime Window & HudOverlay optimization if InbuiltOverlayManager is active
    jclass overlayMgrCls = nullptr;
    {
        std::lock_guard<std::mutex> lock(sJniCacheMutex);
        overlayMgrCls = sOverlayMgrClass;
    }
    if (overlayMgrCls != nullptr) {
        jmethodID getInstMid = env->GetStaticMethodID(
            overlayMgrCls, "getInstance",
            "()Lorg/levimc/launcher/core/mods/inbuilt/overlay/"
            "InbuiltOverlayManager;");
        if (!clearJniException() && getInstMid != nullptr) {
            jobject mgr =
                env->CallStaticObjectMethod(overlayMgrCls, getInstMid);
            if (!clearJniException() && mgr != nullptr) {
                // Optimize HudOverlay (elevation=0f, transparent background, and
                // willNotDraw=true when no external HUD is shown and not in editor)
                jfieldID hudFid = env->GetFieldID(
                    overlayMgrCls, "hudOverlay",
                    "Lorg/levimc/launcher/core/mods/inbuilt/overlay/"
                    "HudOverlay;");
                if (!clearJniException() && hudFid != nullptr) {
                    jobject hudView = env->GetObjectField(mgr, hudFid);
                    if (!clearJniException() && hudView != nullptr) {
                        jclass hudCls = env->GetObjectClass(hudView);
                        jmethodID isEditorMid = env->GetMethodID(
                            hudCls, "isHudEditorMode", "()Z");
                        jmethodID setWillNotDrawMid = env->GetMethodID(
                            hudCls, "setWillNotDraw", "(Z)V");
                        jmethodID setElevationMid =
                            env->GetMethodID(hudCls, "setElevation", "(F)V");
                        jmethodID setBgColorMid = env->GetMethodID(
                            hudCls, "setBackgroundColor", "(I)V");

                        if (!clearJniException() && isEditorMid != nullptr) {
                            const jboolean editorActive =
                                env->CallBooleanMethod(hudView, isEditorMid);
                            clearJniException();
                            if (!editorActive) {
                                if (setBgColorMid != nullptr) {
                                    env->CallVoidMethod(hudView, setBgColorMid,
                                                        0x00000000);
                                    clearJniException();
                                }
                                if (setElevationMid != nullptr) {
                                    env->CallVoidMethod(hudView,
                                                        setElevationMid, 0.0f);
                                    clearJniException();
                                }
                                if (setWillNotDrawMid != nullptr) {
                                    const bool skipCanvasDraw =
                                        moduleEnabled &&
                                        (config.hudOptimizerMode !=
                                         HudOptimizerMode::Off) &&
                                        !config.showHud;
                                    env->CallVoidMethod(
                                        hudView, setWillNotDrawMid,
                                        skipCanvasDraw ? JNI_TRUE : JNI_FALSE);
                                    clearJniException();
                                }
                            }
                        }
                        env->DeleteLocalRef(hudCls);
                        env->DeleteLocalRef(hudView);
                    }
                }
                env->DeleteLocalRef(mgr);
            }
        }
    }

    if (attachedHere) {
        vm->DetachCurrentThread();
    }
#else
    (void)vm;
    (void)config;
    (void)moduleEnabled;
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
