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

---

## Update — VSYNC-off diagnostic: **hypothesis 1 confirmed**, hypothesis 2 ruled out

Per the engine team's suggestion, locally patched `engine/rendering/Renderer.cpp` to clear `BGFX_RESET_VSYNC` from `init.resolution.reset`, rebuilt, ran the figure-8 level for ~10 seconds on Pixel 9.

Seven consecutive samples with **vsync OFF**:

```
frame: full=21.32 begin=0.17 end=20.31 (post=0.00 bgfx=20.28) gameWork=0.84
frame: full=21.18 begin=0.12 end=19.30 (post=0.01 bgfx=19.26) gameWork=1.76
frame: full=24.35 begin=0.26 end=19.72 (post=0.01 bgfx=19.67) gameWork=4.36
frame: full=17.21 begin=0.21 end=15.71 (post=0.00 bgfx=15.68) gameWork=1.30
frame: full=28.21 begin=0.09 end=23.68 (post=0.01 bgfx=23.62) gameWork=4.44
frame: full=19.26 begin=0.09 end=18.89 (post=0.00 bgfx=18.88) gameWork=0.28
frame: full=20.72 begin=0.59 end=18.45 (post=0.00 bgfx=18.42) gameWork=1.68
```

`bgfxFrameMs` is **15.7–23.6 ms** — essentially unchanged from the vsync-on numbers. The variance even widened slightly (no vsync cap holding the upper bound).

**If multi-threaded mode were actually engaged, this is where it would have shown.** A real async hand-off should have returned in ~0.1 ms once the swapchain queue stalling went away.  It did not. The render thread isn't doing the work in the background — `bgfx::frame()` is performing the full submit + GPU wait synchronously on the game thread regardless of `BGFX_RESET_VSYNC`.

### Verdict

**Hypothesis 1 confirmed**: bgfx is silently in single-threaded mode at runtime on Pixel 9 / Android Vulkan, despite:

- `EngineDesc::singleThreaded = false` (verified via our `main_android.cpp`)
- `BGFX_CONFIG_MULTITHREADED=1` in the compiled bgfx flags
- `Renderer::init` correctly skipping the pre-init `bgfx::renderFrame()` call
- bgfx Android Vulkan otherwise initialising cleanly

**Hypothesis 2 ruled out**: vsync-locked queue stall isn't the cause — the numbers don't move when vsync is removed.

### Things worth checking on the engine side

1. **bgfx Android Vulkan render-thread spawn path** — confirm `bgfx::s_ctx->m_renderThread` is actually being created on this device. A `BX_TRACE` at `bgfx.cpp::ContextImpl::run` (the render thread entry) would settle it in seconds; if that trace never fires in the logcat on Pixel 9, we're in single-threaded mode silently.
2. **Whether bgfx's Vulkan backend on Android has a runtime override that forces single-threaded.** Older bgfx versions did exactly this for OpenGL ES; the Vulkan path may inherit the same logic. Worth a `git log` in the bgfx submodule for any Android-specific gating of `BGFX_CONFIG_MULTITHREADED`.
3. **What `apps/perf_smoke/run_both.sh` actually measures on a Pixel 9.** The "expected ~0.1 ms" in `docs/NOTES.md` line 304 came from somewhere — if perf_smoke gets the predicted number on the engine team's device but we don't on ours, there's a device or platform-config difference. If it gets the same ~15+ ms on their Pixel 9 too, the docs entry needs revising.

Patch was reverted locally before this report was written — sama-src is back at clean upstream HEAD.

---

## Update — GPU-strip diagnostic: GPU cost ruled out, hypothesis 1 stands as conclusive

After the vsync-off result, also stripped GPU work to break any remaining "the queue is full because the GPU can't keep up" loophole. Applied in `main_android.cpp` + `SampleGame::onInit`:

- `EngineDesc::shadowResolution = 512` (atlas: 2 MB → 0.5 MB)
- `RenderSettings::shadows.directionalRes = 512`
- `RenderSettings::shadows.filter = Hard` (no PCF)
- `RenderSettings::renderScale = 0.5` (fragment count drops 4×)
- `RenderSettings::lighting.iblEnabled = false`
- `RenderSettings::postProcess.fxaaEnabled = false`
- `RenderSettings::postProcess.bloom.enabled = false`
- `RenderSettings::postProcess.ssao.enabled = false`
- `RenderSettings::depthPrepassEnabled = false`

**Result on Pixel 9, figure-8 level: `bgfx::frame` row still ~15+ ms.** Unchanged.

So bgfx::frame stays ~15+ ms across all three configurations:
1. Default config (vsync on, full GPU) — 15+ ms
2. Vsync off, full GPU — 15+ ms (queue stall hypothesis ruled out)
3. Vsync on, GPU work demolished — 15+ ms (GPU cost ruled out)

The cost is **inside `bgfx::frame()` itself doing the full submit + GPU wait synchronously on the game thread**, regardless of how much GPU work it's actually waiting for and regardless of whether the swapchain is paced. That is the textbook signature of single-threaded mode.

**Final verdict on B2: bgfx is silently single-threaded at runtime on Android Vulkan despite `EngineDesc::singleThreaded = false`, `BGFX_CONFIG_MULTITHREADED=1` in the build, and `Renderer::init` correctly skipping the pre-init `bgfx::renderFrame()` call.**

This is now entirely an engine / bgfx-Android-Vulkan investigation. The fastest path to root-causing it on the engine side: add a one-shot `__android_log_print` at the top of bgfx's `Context::renderThread()` (or wherever it spawns/joins the render thread) and check whether it fires on Pixel 9 init. If it doesn't fire, the render thread isn't being created — that's the bug.

GPU-strip and vsync-off patches have been reverted on sample_game; we're back at the shipping high-tier config so the next sama drop can be tested clean.

---

## Correction — bgfx IS multi-threaded; hypothesis 1 was wrong

Pulled sama `4eed082` which adds two complementary diagnostics:

- `d6325c5 diag(rendering): dump bgfx::getStats() waitSubmit/waitRender + gpu time on Android` — per-frame stats under tag `SamaEngineBgfxStats`.
- `4eed082 diag(rendering): route bgfx BX_TRACE to Android logcat` — wires a `BgfxLogcatCallback` so bgfx's internal `BX_TRACE("Running in %s-threaded mode", …)` reaches logcat under tag `SamaEngineBgfx`.

Built + ran on Pixel 9 (figure-8 level, no GPU strip, vsync on — the shipping config).

### Stats output

```
SamaEngineBgfxStats: frame=120 bgfx::frameMs=15.57 | waitSubmit=0.00 ms | waitRender=13.84 ms | cpu=0.38 ms gpu=3.33 ms | numDraws=21
SamaEngineBgfxStats: frame=240 bgfx::frameMs=16.05 | waitSubmit=0.00 ms | waitRender=15.06 ms | cpu=0.34 ms gpu=3.87 ms | numDraws=21
```

**`waitRender = 13.84 / 15.06 ms` is the conclusive datum**: this field exists only when the submit thread is waiting on a **separate render thread**. If bgfx were single-threaded, `waitRender` would be 0 (there is no render thread to wait for). The render thread exists and is the bottleneck.

So **hypothesis 1 (silently single-threaded) is refuted by direct measurement**. My earlier verdict based on the vsync-off and GPU-strip diagnostics was wrong — both of those tests *did* leave `bgfx::frame` at ~15 ms, but for a reason I didn't consider at the time: the render thread's own latency, not the absence of a render thread.

### What the BX_TRACE callback did not show

The `Running in multi-threaded mode` line never appeared under `SamaEngineBgfx`. Reason: the bgfx build flags include `BX_CONFIG_DEBUG=0`, which expands `BX_TRACE` to a no-op macro at compile time. The callback was correctly installed; nothing was ever sent through it. Fine — the stats numbers above are equally definitive and didn't need it.

### Where the time actually goes

- `bgfx::frameMs` = 15.57 ms — game-thread wall clock inside `bgfx::frame()`.
- `cpu` = 0.38 ms — bgfx's own CPU work on the **submit thread**.
- `gpu` = 3.33 ms — GPU end-to-end.
- `waitRender` = 13.84 ms — game thread blocked waiting for render thread to consume the queue.

15.57 − 0.38 − 13.84 ≈ 1.35 ms — the hand-off overhead.

**The render thread itself is taking ~14 ms per frame.** It's submitting 21 draws of mostly-empty work (3.3 ms of GPU), so the time isn't in command-buffer recording or GPU work itself. The most likely culprit on Android Vulkan: **`vkAcquireNextImageKHR` blocking** while the compositor (SurfaceFlinger) holds onto swapchain images at the display refresh cadence. Even with `BGFX_RESET_VSYNC` cleared, Android's SurfaceFlinger paces presentation, so the acquire blocks ~vsync-period when the swapchain is short.

### New hypotheses for the engine side

1. **Swapchain image count is too low.** Android Vulkan typically allows 2–4 swapchain images. If bgfx requested 2, the render thread can never get ahead — it always blocks on the next acquire. Requesting 3 (or honouring whatever the device reports as `minImageCount + 1`) would give the render thread room to run a frame ahead of the compositor.
2. **`vkAcquireNextImageKHR` is being called inline on the render thread instead of using a fence/semaphore pattern.** A non-blocking acquire (with a semaphore that the next command-buffer waits on) would let the render thread do useful work while the compositor holds an image.
3. **Android's `Choreographer` cadence is the floor**, and the docs' `~0.1 ms` figure came from a desktop/Windows/Mac measurement where SurfaceFlinger isn't in the loop. The docs should call this out so future readers don't expect the same number on Android.

### What would close this

If swapchain image count is the issue, the fix is a 1–3 line change in bgfx's Vulkan renderer init (`renderer_vk.cpp` — request `max(2, minImageCount + 1)` for the swapchain). If the acquire pattern is the issue, that's a larger refactor inside bgfx and probably an upstream PR.

Once the render thread can actually overlap with the compositor, `waitRender` should drop to ≪ 1 ms and `bgfx::frame` collapses to the hand-off cost the docs predict.

---

## Update — sama `6a0cd65` (numBackBuffers=3) did NOT close the gap

Pulled, rebuilt, installed. Verified `init.resolution.numBackBuffers = 3` is in the compiled `Renderer.cpp.o` (source line 191; .o mtime matches the fresh source). Pixel 9 / Android 16 / Vulkan, figure-8 scene, vsync on (shipping config).

### Numbers (worse, not better)

```
SamaEngineBgfxStats: frame=120 bgfx::frameMs=17.15 | waitSubmit=0.00 | waitRender=18.68 | cpu=0.26 | gpu=3.03 | draws=21
SamaEngineBgfxStats: frame=240 bgfx::frameMs=19.01 | waitSubmit=0.00 | waitRender=24.59 | cpu=0.40 | gpu=4.25 | draws=21
```

Baseline from sama `4eed082` (1 commit before this fix) on the same device + scene:
```
bgfx::frameMs=15.57 | waitRender=13.84
bgfx::frameMs=16.05 | waitRender=15.06
```

Frame time regressed by **~2 ms** and waitRender by **~5–10 ms**.

### `Create swapchain` log line didn't appear

Same root cause as the threading-mode line: bgfx is built with `BX_CONFIG_DEBUG=0`, so the `BX_TRACE("Create swapchain numSwapChainImages %u, ...")` at the bgfx call site compiles to a no-op. The `SamaEngineBgfx` callback is correctly installed but receives nothing.

We have no direct confirmation that `numBackBuffers = 3` actually engaged.  Three possibilities ranked by what the data supports:

1. **`maxImageCount` capped to 2** on Pixel 9 — fix didn't take effect, observed change is frame-to-frame noise. A one-line `__android_log_print` right after `bgfx::init` reading the actual swapchain image count (probably via `bgfx::getCaps()` or a fresh BX log site that isn't `BX_TRACE`) would settle this in a single APK rebuild.
2. **3 images engaged, but each extra image cost ~one vsync of compositor-release latency** on this Android version, so the back-pressure win was eaten by added per-image latency. Consistent with the team's "acceptable tradeoff" note but suggests the cost is higher than predicted.
3. **The gate was never swapchain depth** — `vkAcquireNextImageKHR` is called inline-synchronously on bgfx's render thread regardless of swapchain size, so the render thread blocks acquire ~one vsync period whether the swapchain is 2 or 3 deep. Adding an image just adds in-flight latency without unblocking acquire. This is the team's fallback hypothesis 1 (semaphore-driven acquire pattern in `renderer_vk.cpp`).

Hypothesis (3) most cleanly explains *why frame time got slightly worse*: queue depth went up, more frames in flight, more accumulated wait time visible to `waitRender`, but the synchronous acquire still serializes the render thread.

### Next probe that would settle (1) vs (2/3)

A non-`BX_TRACE` print of the bgfx-observed swapchain size right after `bgfx::init` returns — `bgfx::getInternalData()` exposes the underlying `VkSwapchainKHR` on Vulkan, from which `vkGetSwapchainImagesKHR` can return the actual count. ~5 lines.

If the count comes back as 3 → it's hypothesis 2 or 3, and the next investigation is the acquire path inside bgfx (likely an upstream PR rather than a Sama-side change).

If the count comes back as 2 → it's hypothesis 1, the patch wasn't honored, and the next try is to also nudge `BGFX_RESET_MAXANISOTROPY`-style flags or whatever bgfx exposes for swapchain depth in a way the device respects.

`sample_game` is on the latest sama unmodified; happy to rebuild + capture the moment the engine-side reads the actual count.

---

## Settled — `numBackBuffers = 3` DID engage; hypothesis 1 ruled out

External verification via `adb shell dumpsys gfxinfo com.pixelperfect3.samplegame`:

```
GraphicBufferAllocator buffers:
            Handle |         Size |     W (Stride) x H | ... | Requestor
0xb400006fa270ed50 |  9517.50 KiB | 2251 (2256) x 1080 | ... | VRI[NativeActivity]#0(BLAST Consumer)0
0xb400006fa270f430 |  9517.50 KiB | 2251 (2256) x 1080 | ... | VRI[NativeActivity]#0(BLAST Consumer)0
0xb400006fa2710090 |  9517.50 KiB | 2251 (2256) x 1080 | ... | VRI[NativeActivity]#0(BLAST Consumer)0
```

**Three** buffers for the NativeActivity's BLAST Consumer surface. The fix engaged; Pixel 9's `VkSurfaceCapabilitiesKHR::maxImageCount` did not cap to 2.

**Verdict updated:**
- Hypothesis 1 (capped to 2) — **ruled out**.
- Hypothesis 3 (acquire pattern is the gate, not depth) — **strongly favoured** by the data. Going 2 → 3 added in-flight latency (more buffers in the chain → more accumulated `waitRender`) without unblocking the synchronous `vkAcquireNextImageKHR` on the render thread. Frame time *slightly regressed*, which is the signature of "adding latency without removing the serializer."
- Hypothesis 2 (per-image release latency ate the win) — still possible, harder to distinguish from 3 without a Vulkan capture. Both produce the same `waitRender` shape.

### Why the `numSwapChainImages` log didn't appear

bgfx's macro at `bgfx_p.h:48`:

```cpp
#if BX_CONFIG_DEBUG
#  define BX_TRACE  _BGFX_TRACE     // hooked to callback->traceVargs
#  define BX_WARN   _BGFX_WARN
#  define BX_ASSERT _BGFX_ASSERT
#endif
```

When `BX_CONFIG_DEBUG=0` (our build), `BX_TRACE` falls back to bx's default, which is also a no-op when bx itself was built with debug off. The `BX_TRACE("Create swapchain numSwapChainImages %d ...")` at `renderer_vk.cpp:7691` compiles to nothing — our `BgfxLogcatCallback::traceVargs` is wired correctly, but bgfx never calls it.

### Recommended next investigation

Patch `bgfx/src/renderer_vk.cpp` to call `bx::debugPrintf` instead of `BX_TRACE` at the swapchain-create site (one line) so the count, vsync mode, present mode, image format, and min/max ranges are visible regardless of debug flag. Or flip `BX_CONFIG_DEBUG=1` in the bgfx cmake (noisier but turns on every BX_TRACE).

Then the focus moves to the acquire pattern inside bgfx's Vulkan backend:
```cpp
// bgfx renderer_vk.cpp — around m_backBuffer.acquire(...)
const VkResult result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                              acquireSemaphore, VK_NULL_HANDLE, &imageIndex);
```
If this is called inline-synchronously on the render thread before submitting the next frame's commands (rather than using the acquire semaphore to gate the next submit's wait stage), the render thread blocks acquire ~ one vsync regardless of swapchain depth. That's the bgfx-upstream pattern fix.

---

## Update — MAILBOX patch (`089fe5b`) did NOT close the gap

Applied `patches/bgfx_android_mailbox_present.patch` manually to our existing `build/_deps/bgfx_cmake-src` checkout (FetchContent's `PATCH_COMMAND` only fires on initial download; we verified MAILBOX is now first in `s_presentMode` at line 153 post-patch). Rebuilt + installed on Pixel 9.

### Ten consecutive `SamaEngineBgfxStats` samples (figure-8 level, idle ball)

```
frame=720  bgfx::frameMs=9.21  waitSubmit=0.00 waitRender=11.89 cpu=0.21 gpu=2.42 draws=21
frame=840  bgfx::frameMs=24.77 waitSubmit=0.00 waitRender=23.72 cpu=1.77 gpu=2.90 draws=21
frame=960  bgfx::frameMs=10.85 waitSubmit=0.00 waitRender=10.61 cpu=0.19 gpu=2.49 draws=21
frame=1080 bgfx::frameMs=16.86 waitSubmit=0.26 waitRender=19.84 cpu=0.39 gpu=3.09 draws=21
frame=1200 bgfx::frameMs=21.47 waitSubmit=1.32 waitRender=20.39 cpu=0.49 gpu=3.41 draws=21
frame=1320 bgfx::frameMs=14.06 waitSubmit=0.00 waitRender=17.96 cpu=0.22 gpu=3.38 draws=21
frame=1440 bgfx::frameMs=16.20 waitSubmit=0.00 waitRender=17.54 cpu=0.31 gpu=3.46 draws=21
frame=1560 bgfx::frameMs=19.97 waitSubmit=0.00 waitRender=18.69 cpu=0.38 gpu=3.90 draws=21
frame=1680 bgfx::frameMs=17.91 waitSubmit=0.00 waitRender=17.36 cpu=0.31 gpu=3.76 draws=21
frame=1800 bgfx::frameMs=14.85 waitSubmit=0.00 waitRender=13.74 cpu=0.32 gpu=3.58 draws=21
```

Mean `bgfx::frameMs` across the 10 samples: **16.6 ms** — still pinned to the panel's 16.67 ms vsync period. Median 17.4 ms. Variance is wide (9.21 → 24.77 ms) but the average is unchanged from the FIFO baseline.

### Indirect evidence MAILBOX *did* engage

The `Selected present mode` `BX_TRACE` line from the patch never appears in logcat (same `BX_CONFIG_DEBUG=0` issue as before — `BX_TRACE` is compiled out). So we don't have first-party confirmation that MAILBOX is the active present mode.

What the data suggests:

- The 9.21 ms outlier sample is impossible under strict FIFO — FIFO blocks `vkAcquireNextImageKHR` until SurfaceFlinger releases an image, which on this device happens at ~16.7 ms intervals. A real <16.7 ms frame means *some* frames are being released back to bgfx faster than vsync, which is MAILBOX's discard semantic.
- The 24.77 ms outlier sample is consistent with the compositor still gating the long-run cadence at the panel refresh and occasionally costing us two vsyncs for one frame.

Combined picture: MAILBOX appears to be selected, but the compositor (SurfaceFlinger on Android 16 / Pixel 9) is still enforcing refresh-rate gating *at some point in the pipeline* such that the long-run average stays at the vsync period.

### `gfxinfo` after the patch

```
GraphicBufferAllocator buffers:
  0xb400006fa270f010 | 9517.50 KiB | 2251 (2256) x 1080 | VRI[NativeActivity]#0(BLAST Consumer)0
  0xb400006fa270fc70 | 9517.50 KiB | 2251 (2256) x 1080 | VRI[NativeActivity]#0(BLAST Consumer)0
  0xb400006fa27113d0 | 9517.50 KiB | 2251 (2256) x 1080 | VRI[NativeActivity]#0(BLAST Consumer)0
  0xb400006fa2712f50 | 9517.50 KiB | 2251 (2256) x 1080 | VRI[NativeActivity]#0(BLAST Consumer)0
```

4 buffers (the patch's `+ revert swapchain depth change` dropped `numBackBuffers=3` back to bgfx default; the count we see is the Android-side surface buffer chain — BLAST Consumer triple-buffer plus an in-flight image).

### Pointing toward the team's fallback hypothesis 2

> Compositor enforces refresh-rate gating at `vkQueuePresentKHR` itself (would push the wait to `present()` instead of `acquire()`).

This best matches the data:

- *If hypothesis 1 (Tensor driver silently downgrades MAILBOX to FIFO):* every frame would be glued to ~16.7 ms. The 9.21 ms outlier rules that out.
- *If hypothesis 2 (compositor gates at present):* MAILBOX is selected, the render thread successfully acquires images sometimes faster than vsync (the 9.21 ms sample), but `vkQueuePresentKHR` still blocks at refresh cadence so the long-run average stays at the panel period. ✓
- *If hypothesis 3 (something upstream of acquire is the actual blocker):* would need a RenderDoc Android / AGI capture to see which `vk*` call the render thread spends its time in.

The next reasonable engine-side step is to time the render thread's `vkAcquireNextImageKHR`, `vkQueueSubmit`, and `vkQueuePresentKHR` calls individually and post the breakdown — that distinguishes hypothesis 2 from 3 in one APK rebuild.

For the docs entry: the original "~0.1 ms" claim is likely correct on *desktop* Vulkan and possibly correct on Android with `Surface.setFrameRate(refreshRate)` letting the compositor drop frame-pacing enforcement. On Pixel 9 / Android 16 with the stock per-frame `requestedFrameRate: {0.00 Hz}` we see in `dumpsys SurfaceFlinger`, the compositor appears to enforce vsync at present regardless of swapchain configuration.
