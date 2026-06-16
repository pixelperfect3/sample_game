# Perf regression: ~20 ms/frame of unaccounted CPU on Android (Pixel 9), level-2 scene

**Severity:** perf regression (40 FPS where we expect 60)
**Component:** `engine_rendering` — bgfx multi-threaded mode not delivering the predicted win
**Status:** **fix attempted (`0824768`), did not resolve** — `bgfxFrameMs` still ~15+ ms vs predicted ~0.1 ms; need engine-side diagnosis
**First seen:** sama `9b4f123` (2026-05-22) — running on `sample_game` figure-8 level
**Fix attempted:** sama `a2608ec` (and the supporting `0824768` "default bgfx to multi-threaded mode") on **2026-06-16**
**Last known good cadence:** sama `1bfe1ab` (same device, same scene, sustained 60 FPS)
**Reporter:** `sample_game` integration

---

## What we see

`sample_game` figure-8 level on Pixel 9 / Android 16 / Vulkan, captured straight off `engine::rendering::sampleFrameStats()` + game-side `std::chrono` timers around each `IGame` callback:

```
FPS  40.4     CPU 0.34 ms   GPU 3.61 ms   Draws 21   Prims 13725

Pass               CPU(ms)  GPU(ms)
Shadow 0              0.10     0.30
Opaque                0.07       --      (timer artifact on a reconfigured view)
Transparent           0.03     0.01
Tonemap               0.03       --      (new pass since Phase 7)
HUD                   0.02     0.01

Game CPU                ms
onFixedUpdate         0.61
  Physics             0.60
onUpdate              0.00
onRender              0.09
  ShadowSubmit        0.01
  DrawCallUpdate      0.01

Frame total          21.20 ms
  Other/vsync        20.50 ms      ← ~97 % of the frame is here

Entities: 134
```

## The arithmetic

- Game-side CPU (timed with our own `std::chrono` scope-timers): **~0.7 ms** total per frame.
- bgfx CPU submit (from `FrameStats::cpuMs`): **0.34 ms**.
- GPU end-to-end (from `FrameStats::gpuMs`): **3.61 ms**, fully overlapped with CPU.
- Wall-clock frame interval (start of `onRender` N → start of `onRender` N+1): **21.20 ms**.

**Missing ~20 ms** sits between `IGame` callbacks — i.e. inside `Engine::beginFrame`, between systems, in `bgfx::frame()`, or in vsync wait. None of that is timed by anything the game can see.

Scene cost is genuinely small: 134 entities, 21 draws, ~14 K primitives, no skinned meshes, no transparent geometry beyond the ball.

## What changed

Same `sample_game` build, same scene, same device — pulled sama from `1bfe1ab` → `9b4f123`. Frame jumped from a stable 16 ms (60 FPS, vsync-locked) to 20–25 ms (40 FPS).

Commits on `main` between those two points include the Phase 7 unified post-process pipeline (`dbaaed8`), the `LightClusterBuilder` AABB cache (`9b4f123` — *exists as a perf fix*, so the cluster builder is already suspected to be a hotspot), and the engine `FrameStats` work. The new **`Tonemap` pass** visible in the overlay tells us at least one new full-screen blit per frame is happening on this device.

## What we need

Per-system or per-phase CPU breakdown **inside the engine's frame loop**, on Android, on this scene. Something equivalent to the game-side scope-timers but covering:

- `TransformSystem::update`
- `LightClusterBuilder` rebuild / cache check
- `FrustumCullSystem::update`
- `PostProcessSystem::update` (Phase 7 — tonemap submit specifically)
- `bgfx::frame()` (command serialization + GPU sync)
- Vsync wait, if any

A single per-frame log line ("LCB 8.5 ms · PP 5.2 ms · TS 0.3 ms · frame 4.1 ms") would immediately tell us whether the cost is real engine work or wait time. `sample_game` can rebuild against a one-off `engine_*` branch that prints these and post the numbers back here.

## Hypotheses we'd like ruled in/out, in order

1. **Phase 7 post-process is running synchronously on Android Vulkan** with no fast-path skip — the new `Tonemap` pass plus its setup/teardown is adding several ms per frame, even though the scene has nothing post-process-worthy.
2. **`LightClusterBuilder` is rebuilding from scratch every frame** despite the AABB cache landed in `9b4f123` — possibly because the cache invalidates whenever any light moves, and `sample_game` has a directional light whose direction doesn't change but whose computed view matrix might.
3. **`bgfx::frame()` is blocking on GPU sync** for a longer chain than before because Phase 7 added new render targets that the swap can't release until cleared.
4. **Vsync wait** is correctly large and there's no engine regression — we're just doing slightly more work per frame and falling off the 60 Hz cliff to 30/40 Hz. Unlikely given the magnitude but cheap to confirm.

## Repro

`pixelperfect3/sample_game` HEAD (commit `5dfc567` or later) → `./android/build_apk.sh --install` → APK starts directly in level 2 (the debug `kDebugStartLevel = 1` constant) → top-right perf overlay shows the numbers above within ~1 second of startup. Don't even need to tilt the phone.

## Worth checking opportunistically

If a quick fix surfaces, an acceptance bar: same APK should show `Frame total ≤ 16 ms` and `Other/vsync ≤ 14 ms` on this device. That puts it back on the 60 Hz vblank.

---

## Update — engine.frameStats() per-phase numbers (sama `e43ceb0` + game on Pixel 9)

After sama added `Engine::frameStats()` exposing per-phase wall-clock timings, the game logs one line per ~60 frames via `__android_log_print` from `onRender`. Six consecutive samples on the figure-8 level (idle ball, no input):

```
frame: full=33.18 begin=0.13 end=30.94 (post=0.00 bgfx=30.92) gameWork=2.12
frame: full=26.50 begin=0.14 end=24.71 (post=0.00 bgfx=24.66) gameWork=1.65
frame: full=16.45 begin=0.08 end=15.64 (post=0.00 bgfx=15.63) gameWork=0.73
frame: full=30.73 begin=0.11 end=29.06 (post=0.01 bgfx=29.01) gameWork=1.57
frame: full=37.81 begin=0.10 end=36.14 (post=0.00 bgfx=36.10) gameWork=1.57
frame: full=28.32 begin=0.13 end=27.33 (post=0.01 bgfx=27.30) gameWork=0.85
```

| Signal | Observed | What it tells us |
|---|---|---|
| `bgfxFrameMs` | **15.6 – 36.1 ms** every frame, **92–97 % of `fullFrameMs`** | `bgfx::frame()` is dominating. On single-threaded mode the GPU+vsync wait is being charged to the game thread. |
| `postProcessSubmitMs` | 0.00 – 0.01 ms | Phase 7 tonemap pass is effectively free. Hypothesis #1 ruled out. |
| `beginFrameMs` + `gameWork` | 0.21 – 2.27 ms total | Engine work between callbacks and our game CPU are both fine. LightClusterBuilder cache fix in `9b4f123` is doing its job. Hypothesis #2 ruled out. |
| Variance in `fullFrameMs` | 16.45 → 30.73 → 37.81 → 28.32 | Classic vsync-cliff stacking. When work *just barely* fits 16.67 ms we hit 60 Hz; otherwise we drop to 33 ms (30 Hz) or worse. |

**Verdict matches the engine team's row-1 prediction** ("`bgfxFrameMs` ~12–15 ms dominant → bgfx single-threaded mode charging GPU+vsync to the game thread"). The magnitude is actually higher than predicted (15–36 ms), but the shape is identical.

The bgfx multi-threaded-mode flip (`EngineDesc::singleThreaded = false`, mentioned in `docs/ANDROID_SUPPORT.md`) is the right next fix. Hypotheses #1 (post-process) and #2 (LightClusterBuilder cache miss) are off the table.

---

## Update — multi-threaded flip landed (`a2608ec`), still 45-50 FPS on Pixel 9

After pulling sama `a2608ec` and explicitly setting `EngineDesc::singleThreaded = false` from our `main_android.cpp` (which now defines its own `android_main` to override `engine_android`'s default `runner.runAndroid(app)` that wouldn't have set the flag), and confirming `BGFX_CONFIG_MULTITHREADED=1` is in the compiled bgfx flags — the perf overlay still shows:

- **`bgfx::frame` row: ~15+ ms** (vs the engine team's predicted ~0.1 ms in `docs/NOTES.md` "bgfx threading mode — multi-threaded default", line 304: *"expected delta is ~20 ms → ~0.1 ms on the game thread"*)
- **FPS: 45–50** sustained (was 40)

So the flip did *something* — but the dominant cost didn't move where the docs say it should.

### What's verified on our side

- `EngineDesc::singleThreaded` is `false` in our `main_android.cpp` (commit `5b267e7` — our custom `android_main` builds the desc explicitly).
- That value flows through `Engine::initAndroid` → `RendererDesc::singleThreaded = desc.singleThreaded` → `Renderer::init`, where the `if (!desc.headless && desc.singleThreaded) bgfx::renderFrame();` guard correctly **does not** call `bgfx::renderFrame()` before `bgfx::init`.
- bgfx was compiled with `BGFX_CONFIG_MULTITHREADED=1` (`flags.make` in `build/android/arm64-v8a/_deps/bgfx_cmake-build/cmake/bgfx/CMakeFiles/bgfx.dir/`).
- bgfx Android Vulkan path otherwise initialises cleanly (logs `bgfx::init type=9` and renders the figure-8 scene normally).

### Two hypotheses worth checking on the engine side

1. **Multi-threaded mode isn't actually engaging at runtime despite the compile flag and our `false`.** Maybe bgfx's Android Vulkan backend silently falls back to single-threaded for some reason (older bgfx releases had Android-specific overrides). A `bgfx::getCaps()` or the bgfx debug-text overlay would say which mode it's running in. Worth verifying on a Pixel 9 run that matches what was measured for the "expected ~0.1 ms" claim.
2. **Multi-threaded IS engaged, but vsync-locked swapchain (`BGFX_RESET_VSYNC`) keeps the ring queue at depth 1 with a render-thread that's the same cost as a vsync period, so the game thread blocks waiting for queue space.** This is consistent with our number (`bgfx::frame ≈ vsync period − tiny`), and it would mean the docs' "~0.1 ms" measurement was taken with vsync OFF, not the user-shipping configuration.

### What sample_game saw, in the new overlay (Pixel 9, figure-8 level, idle ball)

User report: `bgfx::frame` row showing **15+ ms**; FPS reading 45–50.  Other engine rows expected to be small (`eng begin ≈ 0`, `PostProcess ≈ 0`, `eng end ≈ bgfx::frame + small remainder`), but those numbers haven't been captured in this report yet — happy to update with the full overlay grid if useful.

### What would close this

Either:

- Confirm hypothesis (1) and fix bgfx so multi-threaded engages on Android Vulkan, OR
- Confirm hypothesis (2) and either (a) update `docs/NOTES.md` to clarify that the ~0.1 ms number is vsync-off, or (b) document a recommended swapchain depth / pacing strategy for shipping games that lets multi-threaded mode actually win when vsync is on.

`apps/perf_smoke/run_both.sh` mentioned in the docs entry would give a clean A/B if anyone can run it on a Pixel 9 with vsync on.

### Sample_game current configuration (for reproducibility)

- sama HEAD: `a2608ec` plus our local `__APPLE__` guards on 5 `<TargetConditionals.h>` files (longstanding unrelated bug).
- `EngineDesc{ singleThreaded = false, enableGyro = true }` set in our `main_android.cpp` `android_main`.
- `kEnableAudio = false` due to B1 regression in the perf series (separate report).
- `kDebugStartLevel = 1` → boots straight into figure-8.
- Repro: `./android/build_apk.sh --install` and read the new `bgfx::frame` row of the perf overlay (top-right corner of the screen, tap to toggle).
