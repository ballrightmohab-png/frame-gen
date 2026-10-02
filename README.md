# LeviFrameGen (`FrameGen.levipack`)

**LeviFrameGen v2.6.0 (Fill-to-Cap Honest + Gen-before-Real)** is a native `preload-native` C++20 mod for **Minecraft Bedrock Edition on Android (`arm64-v8a`)** running under **LeviLaunchroid (`LeviLauncher`)**.

It combines **Bidirectional Motion-Compensated GPU Frame Generation (`2×` / `3×` / `4×` Max)** presented as true in-between frames with **Gen-before-Real chronological order** (`A → A.5 → B → B.5 → C`), **Fill-to-Cap pacing (low FPS feels like real)**, **9-Candidate Local Motion & Depth-Parallax Estimation**, **Winner-Take-All Anti-Ghosting Disocclusion Selection**, **Trail-Free Motion Guard**, **Contrast-Adaptive Sub-Texel Sharpening (CAS)**, **Full-Magnitude Perspective-Correct Warp**, **100% Angle-Conserving 3D Perspective Camera Smoothing**, an **outlier-resistant presentation FPS limiter that frame generation can never bypass**, and a **Low-Latency HUD Optimizer** — with **zero screen dimming/blackening** and **zero center-screen logos or overlays**.

> **v2.6.0 highlights — Gen-before-Real + Fill-to-Cap + Honest FPS**
> * **Generated-before-Real (chronologically correct).** Midpoint `A.5` between `A` and `B` is now presented *before* `B` (`A → A.5 → B`), not after. When `B` is rendered, `A.5` is synthesized from `A(prev)` and `B(cur)` then `B` is restored via a blit. This removes one frame of out-of-order judder and keeps block/entity edges razor-sharp.
> * **Fill-to-Cap pacing — low-end feels like real.** The cap still limits the *final presented* FPS (`90 cap + 2× → ≤90`), but when real is below the cap there is **no forced throttle**. `25 natural + 25 generated = 50 presented` feels like `50`, `45+45=90` feels like `90`. A late-frame `skipWaitWhenLate` re-anchors the deadline to `now` instead of adding `0-11 ms` of wait, so low FPS never gets extra latency. High FPS is still throttled (a 200 FPS-capable renderer at `90 cap + 2×` measures `45 +45 =90`).
> * **Honest FPS counter.** Vanilla `FpsMod` shows **real** FPS only, so `300` always feels like `300`. With `90 cap +2×` it shows `45 real` while telemetry shows `45 real +45 gen =90 presented`. No more `300` that feels like `45` or `150+150=300` that feels like `150`.
> * **No motion blur.** Generated frames remain fully opaque single-phase (`k/mult`), not multi-phase averaged or alpha-blended. Ambiguous warps fall back to the sharp real pixel, so fast pans never smear.
> * **Full-magnitude warp + blit.** Perspective-correct flow uses the full motion vector `v` (not `v*confidence`), disciplined by confidence only in the mixer, and real frames are restored via a dedicated blit program (`aPos/aUV/uTex`) with proper sampler/PBO unbind.

> **v2.5 highlights (still included)**
> * **No more motion-blur look.** Generated frames are now presented as their own frames instead of being alpha-blended on top of a real frame, and multi-phase averaging (a literal temporal blur) was removed. `REAL → GENERATED → REAL` is now the actual presentation order.
> * **The FPS cap is respected with frame generation ON.** The limiter now drives the entire presentation pipeline, including generated frames, e.g. `90 FPS cap + 2× FG = ~45 real + ~45 generated = 90 presented FPS`.
> * **Real / Generated / Presented FPS** are reported separately in the Mod Menu telemetry and the optional HUD badge, so the cap is directly verifiable. A **cap integrity monitor** reports the share of recent windows actually held at or below the cap ("held 100%").

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
- **HUD Optimizer Mode**: **`Ultra Low-Latency`**
- **HUD Pre-Swap Latency Flush**: **`ON`**
- **Inventory & GUI Fast-Response Boost**: **`ON`**
- **Smart Auto-Tune Budget Guard**: **`ON`**

---

### 2. Frame Generation FPS Cap & Pacing (Required Reading)

Two separate caps exist so that the limiter can never be bypassed by frame generation:

| Setting | Used when | Behavior |
| :--- | :--- | :--- |
| **FPS Cap with Frame Gen OFF** | Frame generation `Off` | Limits the final presented FPS (plain FPS limiter). |
| **Frame Generation Output FPS** | Frame generation `On` | Limits the **total presented FPS**: real frames **and** generated frames. |

Resolution rule: **the tightest configured cap always wins**. If the regular cap is `60` and the FG output FPS is `120`, the output stays at `60` FPS.

Recommended values (`2×` multiplier):

| Output cap | Real render cadence | Presented frames | Real-frame interval | Present interval |
| :--- | :--- | :--- | :--- | :--- |
| `60 FPS` | ~30 FPS | ~30 real + ~30 generated | 33.33 ms | 16.66 ms |
| `90 FPS` | ~45 FPS | ~45 real + ~45 generated | 22.22 ms | 11.11 ms |
| `120 FPS` | ~60 FPS | ~60 real + ~60 generated | 16.66 ms | 8.33 ms |
| `Unlimited` | as fast as the device allows | native | native | native |

Presentation pacing:
* All timing is driven by a **monotonic high-resolution clock** (`std::chrono::steady_clock`), never by a fixed `sleep()` count.
* Each presented frame has an exact deadline on one uniform presentation grid (11.111 ms at 90 FPS). The pacer sleeps for the coarse part of the wait and yield-spins the final ~200 µs, then hands `eglPresentationTimeANDROID` the absolute deadline so SurfaceFlinger displays real and generated frames evenly spaced.
* If the real renderer falls behind, the grid **re-anchors forward** (missed slots are dropped, never queued), generated frames are reduced, and the full multiplier is restored after ~0.5 s of stable frame times. There is never a presentation backlog or burst catch-up.
* **Input latency:** the pacer is only ever called from the presentation path (`eglSwapBuffers`), after the game has already rendered the real frame. Keyboard/mouse and camera-smoothing hooks never enter the limiter, so the cap never adds input latency.
* The optional HUD badge and the Mod Menu telemetry show **Real**, **Generated** and **Presented** FPS plus the active cap, the real-frame target and a **cap integrity monitor** ("held 100%", violation counter) so you can verify the limiter live.
* **Presented FPS is measured from actual EGL presentations**, never estimated as `real × multiplier`. In the rare composited fallback (a device that refuses extra presented frames) the generated sub-frame is *embedded* inside the real frame, and the counter correctly reports the real presented rate instead of a value above the cap.
* Verified end-to-end: a renderer capable of 200+ FPS forced through a 90 FPS cap with 2:1 FG measures **45.0 real + 45.0 generated = 90.0 presented FPS**.

**Note:** when frame generation is ON, EGL VSync is released (`eglSwapInterval(0)`) because the module owns the presentation clock; set the output cap to your display refresh rate (`60`/`90`/`120`) for the smoothest result.

---

### 3. Best Settings for Maximum Smoothness (`3×` / `4×` Ultra Smooth)
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

### 4. Best Settings for Competitive PvP / Bedwars / Skywars (Instant Flicks)
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

### 5. Best Settings for Budget / Low-End Phones (Battery & Thermal Friendly)
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
| **HUD Optimizer Mode** | `Off`, `Balanced`, `Ultra Low-Latency` | `Ultra Low-Latency` | Flushes GPU HUD queue before swap and eliminates Android UI canvas redraw lag. |
| **HUD Pre-Swap Latency Flush** | `ON` / `OFF` | `ON` | Calls `glFlush()` pre-swap so crosshair and hotbar respond with zero buffered frame delay. |
| **Inventory & GUI Fast-Response Boost** | `ON` / `OFF` | `ON` | Optimizes frame pacing when inventory, chest, or pause menus are open. |
| **Smart Auto-Tune Budget Guard** | `ON` / `OFF` | `ON` | Steps down synthesis workload during heavy chunk-loading spikes. |
| **Present Generated Frames (REAL → GEN → REAL)** | `ON` / `OFF` | `ON` | Presents every generated frame as its own frame with its own presentation timestamp, instead of blending it on top of a real frame (removes the "motion blur" look). `OFF` uses the alpha-composited single-swap fallback. |
| **Trail-Free Motion & Ghost Guard** | `ON` / `OFF` | `ON` | Where the forward/backward warps disagree with no confident winner, keeps the razor-sharp real pixel instead of leaving a smeared motion trail or ghosted double edge. |
| **Frame Generation Output FPS** | `0` (Unlimited), `30`–`240` | `90` | Total presented FPS cap while frame generation is ON. Covers real **and** generated frames; resolution takes the tightest configured cap. |
| **FPS Cap with Frame Gen OFF** | `0` (Unlimited), `30`–`240` | `0` | Plain FPS limiter applied to the presented output while frame generation is disabled. |

---

## Key Features

### 1. Bidirectional Motion-Compensated Frame Generation (`GlFrameInterpolator`)
- **True In-Between Frames (`REAL A -> GENERATED -> REAL B`)**: Warps `REAL A` forward (`uv - p * v_local`) and `REAL B` backward (`uv + (1 - p) * v_local`) toward the midpoint, anchoring every real frame back to ground truth so errors never accumulate.
- **3D Gnomonic (Perspective-Tangent) Camera Flow & 9-Candidate Local Motion Estimation**: Accounts for 3D perspective curvature (`1 + tan²(fov/2) * ndc²`) so fast camera turns never stretch world edges, and evaluates 9 motion candidates per pixel (camera flow, near-block strafe parallax, far-terrain parallax, temporal velocity continuity, stationary, and axial local velocity offsets for moving entities, players, and particles).
- **Winner-Take-All Anti-Ghosting & Border Disocclusion Guard**: Eliminates double blocks, duplicate entities, and motion trails by switching to a single-source confidence selector when `REAL A` and `REAL B` disagree at occlusion boundaries, and automatically selects in-bounds `REAL B` when new terrain enters the screen edge during fast turns.
- **Contrast-Adaptive Sub-Texel Sharpening (CAS)**: Restores crisp Minecraft pixel-art block edges, item models, and textures when bilinear warping samples between texel centers, strictly clamped to the local 5-tap min/max neighborhood for zero ringing or halos.
- **Multi-Tap Static HUD / Crosshair / Hotbar Protection & Zero-Dimming Guard**: Protects the crosshair, hotbar, item counts, text, and FPS counters (`alpha = 0.0` in hardware `GL_ONE_MINUS_SRC_ALPHA` blend), and unbinds OpenGL ES 3.0 `bgfx` sampler/PBO state (`glBindSampler`) with a photometric floor guard so the screen never dims or turns black.
- **Opaque Presented Frames (No Motion Blur)**: In `REAL → GENERATED → REAL` mode each generated frame is drawn as a fully opaque frame at the single exact phase (`k / multiplier`, no multi-phase averaging and no alpha blend with a real frame). Pixels the warp cannot trust (ambiguous disocclusion, HUD, sub-pixel motion, empty readback) fall back to the exact 1:1 real pixel, so generated frames stay as sharp as real frames and never smear.
- **Trail-Free Motion Guard**: Suppresses synthesized detail only where the bidirectional sources disagree with no confident winner — the exact condition that produced motion trails, ghosted player models and double block edges.

### 2. Presentation FPS Limiter & Frame Pacing (`FramePacer`)
- **Single uniform presentation grid** driven by a monotonic high-resolution timer, shared by real and generated frames: real frames land every `presentedInterval × multiplier` ms (22.222 ms at `90 FPS / 2×`), generated frames fill the remaining slots (11.111 ms grid).
- **Frame generation can never bypass the cap**: the resolved cadence takes the tightest of the configured caps and reports *real*, *generated* and *presented* FPS separately.
- **Forward re-anchor instead of queueing**: when the real renderer slips, missed slots are dropped, generated frames are reduced (`cycleDegraded`), and the full multiplier returns after ~0.5 s of stable frame times.
- **Input-latency independence**: the pacer is only entered from `eglSwapBuffers`, after the real frame is rendered; keyboard/mouse, turn-delta and camera-smoothing hooks never wait on it.

### 3. 100% Angle-Conserving Camera Smoother (`CameraSmoother`)
- **Native `libminecraftpe.so` Camera Hooks**: Hooks `LocalPlayer::applyTurnDelta(Vec2 const&)`, `CameraAPI::tryGetFOV`, and `VanillaCameraAPI::getCameraPerspective`.
- **2-Pole Sub-Pixel Residual Reservoir + Fast Combat Flick & Reversal Boost**: Every microradian of input rotation is accumulated in `double` precision and subtracted only as it is emitted to the game, guaranteeing **100.000% angle conservation**, **zero stationary drift**, and instant response during 180° turns and combat strafing.

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
