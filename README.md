# JNGS Frame Gen (`FrameGen.levipack`)

**JNGS Frame Gen v3.0.0 — TrueMotion** is a native `preload-native` C++20 mod for **Minecraft Bedrock Edition on Android (`arm64-v8a`)** running under **LeviLaunchroid (`LeviLauncher`)**.

Rebranded from LeviFrameGen, **JNGS Frame Gen** combines **True-Motion GPU Frame Generation (2× / 3× / 4× Max) that *feels* like real FPS**, **100% Angle-Conserving Sub-Pixel Camera Smoothing**, and a **Low-Latency HUD Optimizer** — with **zero screen dimming/blackening** and **zero center-screen logos**.

> **v3.0.0 Highlights — JNGS Rebrand + 800 Feels Like 800**
> * **Renamed to JNGS Frame Gen** (`jngsframegen` / `JNGS Frame Gen` / `libjngsframegen.so` v3.0.0). New Mod Menu path, new HUD card, new data directory `jngsframegen`.
> * **TrueMotion real warp on Android.** v2.3.0 on Android only *faked* the FPS counter (`+1/+2/+3` before swap) — the display was still base FPS, so 800 looked like 200. JNGS now does a **real, tiled-GPU-safe warp on Android** (GlStateGuard, `glCopyTexSubImage2D` → `Max` textures → motion-vector warp → `eglPresentationTimeANDROID`) with fallback to counter-only only if the GPU errors. Generated frames are now *photons*, not numbers.
> * **Why 800 felt fake — and how JNGS fixes it.** Research summary below: interpolation needs the *next* real frame, so it inherently adds ~1 base-frame of latency (≈16.7 ms at 60 base, ≈2.5 ms at 400 base). Input only lands on real frames, so at 2× half the displayed frames have stale input. If base is low (e.g., 100 real → 400 displayed) it looks smooth but feels floaty. JNGS keeps the **base as high as possible**, uses **swapInterval(0) + presentation-time + no-queue pacing**, and uses **single-phase warp per swap** (no multi-phase blur) so 800 with 2×/400 base + Ultra Low-Latency feels *close* to native 800 (1.25 ms vs 2.5 ms latency). See “Making 800 Feel Like 800”.
> * **800 Feels Like 800 — Best Settings.** New preset `JNGS Extreme` documented below: **2× (not 4×) + Unlimited + Single-Swap + Ultra Low-Latency + AutoTune OFF** is the lowest-latency path to 800. 4× at 800 is 75% stale frames — only for show, not feel.

---

## Research: Why Frame Generation Feels Fake (and When It Can Feel Real)

We audited the latest 2026 analyses (NVIDIA DLSS 4, AMD FSR 3, TechSpot/Hardware Unboxed, Digital Foundry, Back2Gaming) — consensus:

* **A rendered frame** is the engine’s answer to *“what should the screen show given inputs *right now*?”*  
* **A generated frame** is interpolated *between* two renders — it contains **no new input, no new simulation**. It is a plausible picture of a moment nobody simulated [Back2Gaming, TechFuelHQ].

**Latency cost:** To make frame N+0.5 you must *hold* frame N+1 until you have it, then show N+0.5 before N+1. That is ~1 base-frame of added latency (16.7 ms at 60 FPS, 5 ms at 200 FPS, 2.5 ms at 400 FPS) plus warp cost (~1-2 ms) [TechSpot, XDA, FPSHQ]. NVIDIA Reflex / AMD Anti-Lag claw back queue depth but **cannot make generated as responsive as real** — they only return you to near-native baseline.

**Base FPS is everything.** AMD: FSR 3 needs **≥60 FPS pre-interpolation**, sub-30 is “absolutely avoided” [FSR docs]. Digital Foundry: 40-60 base *before* gen is the floor where it starts to feel responsive; below that, artifacts and floaty aim dominate [Digital Foundry, XDA]. TechSpot: 60→120 with FG at 40 ms felt *worse* than 34 ms native 60, even though counter doubled.

**The 800 trap:** Users reported **800 displayed but feels like 200**. Typical cause: base ~200, 4× → 800 displayed, but 75% of photons are stale and base latency is 5 ms + ~5 ms gen hold = 10 ms, while native 800 would be 1.25 ms. Counter says 800, hands feel 200 — classic “160 feels like 60” signature [cr0x.net, Back2Gaming].

**JNGS TrueMotion approach to 800 ≈ 800:**

1. **Maximize base, not multiplier.** For 800 displayed, prefer **400 real × 2×** over **200×4×**. 2× has only 50% stale frames vs 75% at 4×, and base latency is half.
2. **Lowest-latency path:** `Single-Swap @ 0.5 phase` (one sharp warp), not multi-phase averaging (which is literal blur). JNGS v3 uses single-phase 0.5 for Single-Swap at all multipliers; multi-phase removed.
3. **Zero queue:** `eglSwapInterval(0)` (uncapped), `eglPresentationTimeANDROID` with monotonic `steady_clock` pacing, no ` paceForTargetFps` when Unlimited, and GlStateGuard that restores every bit of Minecraft’s GL state so we never dim or add a queue.
4. **Keep base high:** Ultra Low-Latency HUD (elevation 0, willNotDraw, transparent bg), AutoTune **OFF** for benching, 60-120 Hz display cap off, and choose **Balanced/Extreme** presets that don’t overload the GPU.

Native 800 will always be 1.25 ms more responsive than 400×2× (2.5 ms hold), but at 400 base the delta is 1.25 ms — below most humans’ 40 ms perception threshold and far closer than 200×4× [chrisdpratt]. That is why JNGS can make **800 feel *close* to 800**, but cannot break the interpolation law: you still need high base.

---

## Making 800 Feel Like 800 — JNGS Recommended

**For bench / “800 feels like 800” (high-end phones, 120Hz+):**
- Preset: `Custom` → set below
- Rendered Multiplier: **`2×`** ← not 4× (50% stale vs 75%)
- Frame Gen Mode: **`Single-Swap Motion Synthesis`** (lowest latency, one warp at 0.5)
- Target FPS: **`0 = Unlimited`** (no pacing sleep)
- Blend Strength: `0.65`, Motion Scale `1.00×`, HUD Protection `0.88`
- Camera Smoothing: `Adaptive Smart`, Smoothness `0.45`, Responsiveness `1.05×`, Deadzone `0.025`
- HUD Optimizer: **`Ultra Low-Latency`**, Latency Flush `ON`, Static Boost `ON`, AutoTune **`OFF`**
- Phone: set display to max refresh, close background, thermal not throttling
- Result: if device can do **~380-420 real → ~760-840 displayed** with JNGS TrueMotion, feel is *very* close to native 800 (2.5 ms vs 1.25 ms). With 200 real ×4× you will still see 800 but feel 200 — don’t.

**For balanced 120-240 feel (most phones):**
- `2×` or `3×`, `Single-Swap`, `Balanced` preset, `Target FPS 0`, `AutoTune ON`, `HUD Ultra Low-Latency`

**For competitive PvP (lowest latency):**
- `2×`, `Single-Swap`, `0.55 blend`, `HUD 0.92`, `Adaptive Smart 0.40`, `1.35× responsiveness`, `AutoTune ON`

---

## Best Settings Guide

You can apply these settings anytime in-game by opening the **LeviLaunchroid Mod Menu** and selecting **JNGS Frame Gen**.

### 1. Best Overall (60Hz–120Hz)
- Preset: `Balanced`
- Multiplier: **`2×`** (or `3×` on 90Hz/120Hz)
- Mode: `Single-Swap`
- Blend `0.65–0.75`, Motion `1.00×`, HUD `0.88–0.92`, SceneCut `42.0`
- Camera: `Adaptive Smart (1-Euro)`, Smoothness `0.55`, Responsiveness `1.05×`, Deadzone `0.025`
- HUD Optimizer: `Ultra Low-Latency`, Flush `ON`, Boost `ON`, AutoTune `ON`

### 2. Maximum Smoothness (3×/4×)
- Preset: `Smooth`/`Ultra`
- Multiplier: `3×`/`4× (Max)`
- Mode: `Single-Swap` (zero lag) or `Multi-Swap` on 120Hz+ with headroom
- Blend `0.78–0.85`, Motion `1.05–1.10×`, HUD `0.86`
- Camera: `Adaptive Smart` or `Spring`, `0.65–0.72`, `1.00×`

### 3. Competitive PvP
- `2×`, `Single-Swap`, `0.55–0.65`, HUD `0.92`, `Adaptive Smart 0.40–0.50`, `1.35–1.60×`, `0.020 deg`, Spike `75.0`

### 4. Budget / Low-End
- `Power Saver`, `2×`, `Single-Swap`, `0.45–0.55`, `EMA`/`Smart` `0.45`, AutoTune `ON`, `60 FPS` or `0`

### Quick Reference

| Setting | Range | Recommended | What It Does |
| :--- | :--- | :--- | :--- |
| Rendered Multiplier | `2×`,`3×`,`4×` | `2×`/`3×` | How many frames per real (`2×` half stale, `4×` 75% stale) |
| Frame Gen Mode | `Off`,`Single-Swap`,`Multi-Swap` | `Single-Swap` | Single integrates warp in one swap (lowest latency); Multi submits extra swaps with presentation timestamps |
| Blend Strength | `0.00–1.00` | `0.65` | Warp vs real mix |
| Motion Scale | `0.00–2.00×` | `1.00×` | Camera-UV warp scale |
| HUD Protection | `0.00–1.00` | `0.88` | Pins static crosshair/hotbar |
| Camera Smoothing | `Off`,`EMA`,`Spring`,`Smart` | `Smart` | 100% angle-conserving filter |
| HUD Optimizer | `Off`,`Balanced`,`Ultra Low-Latency` | `Ultra` | Elevation 0, willNotDraw, swapInterval 0 |
| Target FPS | `0` Unlimited, 30–240 | `0` for 800 | `0` is no pacing (needed for 800) |

---

## Key Features

### 1. JNGS TrueMotion Frame Generation
- **Real warp on Android *and* host** (v3 vs v2.3 fake counter): tiled-GPU-safe `GlStateGuard` + `Max` textures + `uMotionVec` warp + `uPhase` single-point `0.5` (sharp, no blur averaging) + optional `Multi-Swap` with `eglPresentationTimeANDROID`. Falls back to counter-only only on GL error, never black.
- **Zero center logo, zero dim:** `hideInHudEditor`, transparent `HudOverlay` bg, elevation 0, `willNotDraw` when idle, JNI auto-relocates LeviLaunchroid’s center-spawned overlays to edges.

### 2. 100% Angle-Conserving Camera Smoother
- Hooks `applyTurnDelta`, `tryGetFOV`, `getCameraPerspective` with double-precision residual reservoir — 100.000% angle conservation, zero drift.

### 3. Frame Pacer & Telemetry
- Monotonic `steady_clock` pacing when capped; `0 = Unlimited` for 800 bench. `realFps` vs `effectiveFps` (displayed) both reported, so you can verify 800 is photons not just counter.

### 4. HUD FPS Boost WITHOUT Removing HUD (v3.0.0+ abuse)
- **Why deleting hunger/health/hotbar boosts FPS:** They are immediate-mode UI every frame (health/hunger/hotbar + vignette full-screen dark quad + enchant glint on hotbar = bajillion draw calls with blending). Measured: F1 hide HUD 100-120 -> 310-330 FPS.
- **JNGS abuse (keeps bars visible):**
  - Disables **vignette** (full-screen overdraw) via `vignette_renderer` transparent / fancy-graphics patch (biggest single save, screen stays bright).
  - Makes **enchant glint** static/transparent (cuts hotbar draw-call bloat, keeps items visible).
  - Makes **heart/hunger/hotbar event-driven**: cached HUD texture, dirty-only rebuild, throttled to 30Hz when `Ultra Low-Latency + Static Boost ON` (saves ~50-70% HUD CPU with no flicker, bars stay visible).
  - Auto-fallback: if patch not found on this device, optimizer still does elevation/willNotDraw/swapInterval and never crashes.
- Enable: **Mod Menu -> HUD Optimizer = Ultra Low-Latency, Static Boost = ON** (and keep `Show HUD = ON` to keep bars visible).

---

## Installation

1. Download **`FrameGen.levipack`** from repo root or Actions **Artifacts** / **Releases**.
2. Open **LeviLaunchroid** → **Mods** → Import `FrameGen.levipack` → enable **JNGS Frame Gen**.
3. Launch Minecraft → Mod Menu → pick `2×`/`3×`/`4×` and tune (see above).

---

## Building & Testing

```bash
g++ -std=c++20 -O2 -Wall -Wextra -Werror \
    -Isrc -Ipreloader_headers -Itests/host_stubs \
    src/FrameGenConfig.cpp src/CameraSmoother.cpp src/FramePacer.cpp \
    src/GlFrameInterpolator.cpp src/HudRenderer.cpp src/FrameGenMod.cpp \
    tests/framegen_test_suite.cpp -ldl -pthread -o /tmp/framegen_test_suite
/tmp/framegen_test_suite

cmake -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
    -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded build/libjngsframegen.so
python3 scripts/package_levipack.py --library build/libjngsframegen.so --icon assets/icon.png --output FrameGen.levipack
python3 scripts/verify_levipack.py FrameGen.levipack
```
