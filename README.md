# LeviFrameGen (`FrameGen.levipack`)

**LeviFrameGen v2.0** is a native `preload-native` C++20 mod for **Minecraft Bedrock Edition on Android (`arm64-v8a`)** running under **LeviLaunchroid (`LeviLauncher`)**.

It combines **GPU Frame Generation & Motion-Compensated Temporal Synthesis** (`libEGL.so` / `GLESv2`) with **100% Angle-Conserving Sub-Pixel Camera Smoothing** (`libminecraftpe.so` native camera detours) and full **LeviLaunchroid Mod Menu UI (V1 + V2 Schema + HUD Editor + Floating Quick-Preset Button)** integration.

---

## Key Features

### 1. GPU Frame Generation & Temporal Synthesis (`GlFrameInterpolator`)
- **Native `eglSwapBuffers` Hook**: Hooks `eglSwapBuffers` in `libEGL.so` and validates `eglGetCurrentContext()`, surface dimensions (`EGL_WIDTH` / `EGL_HEIGHT`), and `EGL_RENDER_BUFFER == EGL_BACK_BUFFER (0x3084)` so offscreen pbuffers and auxiliary surfaces are never corrupted.
- **Two Synthesis Modes**:
  - **Single-Swap Motion Synthesis (Safe Default)**: Synthesizes a motion-compensated, HUD-aware frame directly in the backbuffer before presentation using a 5-tap bidirectional optical-flow reconstruction shader and 5-tap contrast-adaptive sharpening (CAS). Works cleanly across all Android Adreno/Mali/Xclipse drivers without SurfaceFlinger queue stalls.
  - **Multi-Swap Display Interpolation (`2x` / `3x` / `4x`)**: Presents `M-1` intermediate synthesized sub-frames with strictly monotonic `eglPresentationTimeANDROID` timestamps (`tPrev + (k/M) * frameIntervalNs`) followed by the real frame on high-refresh-rate (`90Hz` / `120Hz` / `144Hz`) displays.
- **HUD & Crosshair Ghosting Protection (`preserveHudCrosshair`)**: Detects high-saturation/high-contrast UI elements and crosshair pixels in the fragment shader and pins them to the latest rendered frame so hotbars, menus, and crosshairs never ghost during fast camera turns.
- **Scene-Cut & Teleport Guard (`sceneCutProtection`)**: Automatically resets temporal frame history on dimension changes, respawns, F5 camera perspective toggles, FOV snaps, or large camera flicks above the configurable cut threshold.
- **Strict `GlStateGuard` RAII Restoration**: Saves and restores all 16 OpenGL ES bindings (`GL_FRAMEBUFFER_BINDING`, `GL_CURRENT_PROGRAM`, `GL_ARRAY_BUFFER_BINDING`, `GL_ELEMENT_ARRAY_BUFFER_BINDING`, `GL_ACTIVE_TEXTURE`, `GL_TEXTURE_BINDING_2D` on units 0 & 1, `GL_VIEWPORT`, `GL_SCISSOR_BOX`, `GL_BLEND` + `glBlendFuncSeparate`, `GL_DEPTH_TEST`, `GL_SCISSOR_TEST`, `GL_CULL_FACE`, `GL_STENCIL_TEST`, and `GL_COLOR_WRITEMASK`) so Minecraft's RenderDragon pipeline is never disturbed.

### 2. 100% Angle-Conserving Camera Smoother (`CameraSmoother`)
- **Direct `libminecraftpe.so` Camera Hooks**:
  - `LocalPlayer::applyTurnDelta(Vec2 const&)` via ARM64 signature resolution.
  - `CameraAPI::tryGetFOV` (`9CameraAPI` vtable index `7`) for automatic zoom-aware smoothing.
  - `VanillaCameraAPI::getCameraPerspective` (`16VanillaCameraAPI` vtable index `7`) for instant F5 perspective-cut detection.
- **Zero Lost Aim / Zero Stationary Drift**:
  - Uses a **2-pole sub-pixel residual reservoir (`double` precision)**. Every microradian of touch/mouse/controller input enters the reservoir and is Subtracted only as it is emitted to `LocalPlayer::applyTurnDelta`. Total camera rotation is **100.000% conserved** with zero stationary drift.
- **Three Selectable Filter Laws**:
  - **Adaptive Smart (1-Euro Filter)**: Velocity-adaptive 2-pole filter with derivative low-pass filtering and soft-knee fast-flick boost. Heavy jitter elimination during fine aiming; near-zero latency on fast 180° combat flicks.
  - **Critically Damped Spring**: Analytical critically-damped harmonic oscillator ($\zeta = 1.0$) with zero overshoot.
  - **Exponential (2-Pole Low-Pass)**: Classic cascaded exponential smoothing with frame-rate-independent time constant $\tau$.
- **Hysteresis Micro-Deadzone (`microDeadzone`)**: Suppresses touch digitizer polling noise while stationery without swallowing intentional slow diagonal aim.

### 3. In-Game LeviLaunchroid Mod Menu UI (`FrameGenConfig` & `HudRenderer`)
- **Dual V1 + V2 ModMenu Registration**:
  - Registers all controls via `pl::modmenu::ModuleBuilder` (V1) **and** a rich categorized `pl::modmenu::ConfigSchemaBuilder` (V2 JSON schema) with Segmented/Chip selectors, sliders with units, conditional visibility (`visibleWhen`), live telemetry badges, and instant action buttons (`Reset Camera State`, `Restore Recommended Defaults`).
- **5 Built-In Profiles**: `Power Saver`, `Balanced (Recommended)`, `Smooth`, `Ultra`, and `Custom` (automatically selected when tuning individual sliders).
- **Live Telemetry HUD Overlay + HUD Editor Support**:
  - Renders a customizable overlay card (`Detailed`, `Compact`, or `Minimal`) via `pl::modmenu::submitDrawCommands` showing Effective FPS, Real FPS, 1% Low FPS, Frame Time (`ms`), Jitter (`ms`), active FrameGen multiplier, and Camera Smoothing mode.
  - Registers a draggable/snappable `HudEditorElement` via `pl::modmenu::submitHudEditorElements` so the HUD card can be repositioned directly in LeviLaunchroid's HUD Editor.
- **Floating Quick-Preset Button & Hotkeys**:
  - Optional floating SVG badge button (`pl::modmenu::registerButton`) to cycle presets with one tap in-game.
  - Keyboard/controller shortcuts: `F8` toggles LeviFrameGen, `F9` cycles presets.

---

## Installation

1. Download **`FrameGen.levipack`** from the repository root or the GitHub Actions **Artifacts** / **Releases** tab.
2. Open **LeviLaunchroid (`LeviLauncher`)** on your Android device.
3. Import **`FrameGen.levipack`** in the **Mods** tab and enable **FrameGen & Camera Smoother**.
4. Launch Minecraft Bedrock and open the **LeviLaunchroid Mod Menu** overlay to customize presets, frame generation, camera smoothing, and the live FPS HUD.

---

## Building & Testing

### Run Host Verification & Simulation Suite (Linux `x86_64`)
```bash
g++ -std=c++20 -O2 -Wall -Wextra -Werror \
    -Isrc -Ipreloader_headers -Itests/host_stubs \
    src/FrameGenConfig.cpp \
    src/CameraSmoother.cpp \
    src/FramePacer.cpp \
    src/GlFrameInterpolator.cpp \
    src/HudRenderer.cpp \
    src/FrameGenMod.cpp \
    tests/framegen_test_suite.cpp \
    -ldl -pthread -o /tmp/framegen_test_suite
/tmp/framegen_test_suite
```

### Build Android `arm64-v8a` (`FrameGen.levipack`) with Android NDK
```bash
cmake -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-26 \
    -DANDROID_STL=c++_shared \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded build/libframegen.so
python3 scripts/package_levipack.py --library build/libframegen.so --icon assets/icon.png --output FrameGen.levipack
python3 scripts/verify_levipack.py FrameGen.levipack
```
