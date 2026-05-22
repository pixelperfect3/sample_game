# Perf regression: ~20 ms/frame of unaccounted CPU on Android (Pixel 9), level-2 scene

**Severity:** perf regression (40 FPS where we expect 60)
**Component:** unknown — between `IGame` callbacks in `engine_core` / `engine_rendering`
**Status:** open
**First seen:** sama `9b4f123` (2026-05-22) — running on `sample_game` figure-8 level
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
