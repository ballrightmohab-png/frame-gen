# LeviFrameGen (`FrameGen.levipack`)

**LeviFrameGen v2.1** is a native `preload-native` C++20 mod for **Minecraft Bedrock Edition on Android (`arm64-v8a`)** running under **LeviLaunchroid (`LeviLauncher`)**.

It combines **GPU Frame Generation (`2×` / `3×` / `4×` Max)** with **100% Angle-Conserving Sub-Pixel Camera Smoothing** and full **LeviLaunchroid Mod Menu UI** configuration.

---

## Best Settings Guide (Recommended Configurations)

You can apply these settings anytime in-game by opening the **LeviLaunchroid Mod Menu** and selecting **LeviFrameGen**.

### 1. Best Overall Settings (Recommended for Most Phones — 60Hz to 120Hz)
This gives the cleanest balance of high perceived frame rate, zero HUD/crosshair ghosting, and ultra-responsive camera movement:
- **Performance & Smoothness Preset**: `Balanced` (or `Custom` with the values below)
- **Rendered Frame Multiplier**: **`2×`** (or **`3×`** on 90Hz/120Hz displays)
- **Frame Generation Mode**: `Single-Swap Motion Synthesis` *(zero SurfaceFlinger queue jitter)*
- **Temporal Synthesis Strength**: **`0.65` – `0.75`**
- **Optical Motion Vector Scale**: **`1.00×`**
- **HUD & Crosshair Edge Protection**: **`0.88` – `0.92`**
- **Scene-Cut Reset Threshold**: **`42.0 deg/f`**
- **Camera Smoothing Algorithm**: `Adaptive Smart (1-Euro)`
- **Camera Smoothing Strength**: **`0.55`**
- **Fast-Turn Responsiveness Gain**: **`1.05×`**
- **Touch Micro-Jitter Deadzone**: **`0.025 deg`**
- **FOV-Aware Zoom Sensitivity Scaling**: **`ON`**
- **Smart Auto-Tune Budget Guard**: **`ON`**

---

### 2. Best Settings for Maximum Smoothness (`3×` / `4×` Ultra Smooth)
Best for **90Hz / 120Hz / 144Hz** screens, survival worlds, shaders, or cinematic gameplay where you want the highest possible fluidity:
- **Performance & Smoothness Preset**: `Smooth` (`3×`) or `Ultra` (`4×`)
- **Rendered Frame Multiplier**: **`3×`** or **`4× (Max)`**
- **Frame Generation Mode**:
  - Choose **`Single-Swap Motion Synthesis`** if you want zero display-buffer lag.
  - Choose **`Multi-Swap Frame Interpolation`** on 120Hz/144Hz phones if your GPU has headroom for extra swap passes.
- **Temporal Synthesis Strength**: **`0.78` – `0.85`**
- **Optical Motion Vector Scale**: **`1.05×` – `1.10×`**
- **HUD & Crosshair Edge Protection**: **`0.86`**
- **Camera Smoothing Algorithm**: `Adaptive Smart (1-Euro)` or `Critically Damped Spring`
- **Camera Smoothing Strength**: **`0.65` – `0.72`**
- **Fast-Turn Responsiveness Gain**: **`1.00×`**
- **Touch Micro-Jitter Deadzone**: **`0.030 deg`**

---

### 3. Best Settings for Competitive PvP / Bedwars / Skywars (Instant Flicks)
Tuned to eliminate touch digitizer jitter while keeping 180° combat flicks instantaneous:
- **Rendered Frame Multiplier**: **`2×`** (or **`3×`**)
- **Frame Generation Mode**: `Single-Swap Motion Synthesis`
- **Temporal Synthesis Strength**: **`0.55` – `0.65`**
- **HUD & Crosshair Edge Protection**: **`0.92`** *(keeps crosshair 100% locked)*
- **Camera Smoothing Algorithm**: `Adaptive Smart (1-Euro)`
- **Camera Smoothing Strength**: **`0.40` – `0.50`**
- **Fast-Turn Responsiveness Gain**: **`1.35×` – `1.60×`** *(ramps cutoff frequency immediately during fast flicks)*
- **Touch Micro-Jitter Deadzone**: **`0.020 deg`**
- **Turn-Spike Clamp**: **`75.0 deg/f`**

---

### 4. Best Settings for Budget / Low-End Phones (Battery & Thermal Friendly)
Ideal when your phone heats up or drops frames in heavy chunks:
- **Performance & Smoothness Preset**: `Power Saver`
- **Rendered Frame Multiplier**: **`2×`**
- **Frame Generation Mode**: `Single-Swap Motion Synthesis`
- **Temporal Synthesis Strength**: **`0.45` – `0.55`**
- **Camera Smoothing Algorithm**: `Exponential (EMA)` or `Adaptive Smart (1-Euro)`
- **Camera Smoothing Strength**: **`0.45`**
- **Smart Auto-Tune Budget Guard**: **`ON`** *(automatically scales workload if frame time spikes)*
- **Frame Pacer Target FPS**: **`60 FPS`** *(or `0` for native VSync)*

---

### Quick Reference Table: What Each Setting Does

| Setting | Range / Options | Recommended | What It Does |
| :--- | :--- | :--- | :--- |
| **Rendered Frame Multiplier** | `2×`, `3×`, `4×` (Max) | `2×` or `3×` | Chooses how many frames are generated per real frame (`2×` Double, `3×` Triple, `4×` Quad Max). |
| **Frame Generation Mode** | `Off`, `Single-Swap`, `Multi-Swap` | `Single-Swap` | `Single-Swap` integrates sub-frame motion inside Minecraft's native swap; `Multi-Swap` submits `2×`–`4×` swaps with presentation timestamps. |
| **Temporal Synthesis Strength** | `0.00` – `1.00` | `0.65` | Controls how strongly motion-compensated sub-frames are blended. |
| **Optical Motion Vector Scale** | `0.00×` – `2.00×` | `1.00×` | Scales camera-velocity-to-screen-UV optical flow warping. |
| **HUD & Crosshair Edge Protection** | `0.00` – `1.00` | `0.88` | Detects static HUD/hotbar/crosshair pixels and pins them to prevent ghosting. |
| **Camera Smoothing Algorithm** | `Off`, `EMA`, `Spring`, `Adaptive Smart` | `Adaptive Smart` | Selects the 100% angle-conserving camera filter law. |
| **Camera Smoothing Strength** | `0.00` – `0.95` | `0.55` | Higher values make slow camera pans silkier. |
| **Fast-Turn Responsiveness Gain** | `0.10×` – `2.50×` | `1.05×` | Higher values make fast combat flicks snap immediately with zero lag. |
| **Touch Micro-Jitter Deadzone** | `0.000` – `0.500 deg` | `0.025 deg` | Filters out stationary touchscreen polling noise without losing slow aim. |
| **Smart Auto-Tune Budget Guard** | `ON` / `OFF` | `ON` | Steps down synthesis workload during heavy chunk-loading spikes. |

---

## Key Features

### 1. Selectable `2×`, `3×`, and `4×` (Max) GPU Frame Generation (`GlFrameInterpolator`)
- **Direct `2×` / `3×` / `4×` Selector in Mod Menu**: Pick `2×` (Double FPS), `3×` (Triple FPS), or `4×` (Quad FPS — Max) right from the top of the Mod Menu.
- **Works in Both Single-Swap and Multi-Swap Modes**:
  - **Single-Swap Motion Synthesis (`2×` / `3×` / `4×`)**: Integrates 1-phase (`2×`), 2-phase (`3×`), or 3-phase (`4×`) bidirectional optical-flow samples in a single native swap pass for zero SurfaceFlinger queue latency.
  - **Multi-Swap Frame Interpolation (`2×` / `3×` / `4×`)**: Submits `1`, `2`, or `3` intermediate synthesized frames plus the final frame (`2×`, `3×`, or `4×` total swaps) with monotonic `eglPresentationTimeANDROID` timestamps.
- **No Center-Screen Logo**: Clean in-game view with zero floating logo clutter in the middle of the screen; optional top-left telemetry HUD card can be toggled or dragged anywhere via the LeviLaunchroid HUD Editor.

### 2. 100% Angle-Conserving Camera Smoother (`CameraSmoother`)
- **Native `libminecraftpe.so` Camera Hooks**: Hooks `LocalPlayer::applyTurnDelta(Vec2 const&)`, `CameraAPI::tryGetFOV`, and `VanillaCameraAPI::getCameraPerspective`.
- **2-Pole Sub-Pixel Residual Reservoir**: Every microradian of input rotation is accumulated in `double` precision and subtracted only as it is emitted to the game, guaranteeing **100.000% angle conservation** and **zero stationary drift**.

---

## Installation

1. Download **`FrameGen.levipack`** from the repository root or the GitHub Actions **Artifacts** / **Releases** tab.
2. Open **LeviLaunchroid (`LeviLauncher`)** on your Android device.
3. Import **`FrameGen.levipack`** in the **Mods** tab and enable **LeviFrameGen**.
4. Launch Minecraft Bedrock and open the **LeviLaunchroid Mod Menu** to pick `2×`, `3×`, or `4×` and tune your settings.

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
