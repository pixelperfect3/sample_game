# Sama Engine Proposals & Bug Reports

Design proposals and bug reports for the [Sama engine](https://github.com/pixelperfect3/sama) that originated from `sample_game`. Each entry is a self-contained doc — written so it can be sent upstream as a PR description, GitHub issue body, or RFC without further editing.

This file is the **index**. Individual entries live as separate files in `proposals/` and `bug-reports/`.

---

## Active proposals

| # | Proposal | Status | Targets | Summary |
|---|---|---|---|---|
| 1 | [Compound & Mesh Collider Shapes](proposals/physics-shapes.md) | proposal | `engine_physics` | Allow one rigid body to represent N convex children (`StaticCompoundShape`) or a triangle soup (`MeshShape`), referenced by shape ID. Eliminates the N-entities-for-one-piece-of-geometry pattern (e.g. figure-8 floor: 128 colliders → 1). |
| 2 | [Close the bgfx Abstraction Boundary](proposals/bgfx-abstraction.md) | adopted | `engine_rendering` | Three small additions (`RenderPass::name/clearColor/clearNone`, engine-side default view naming, new `FrameStats` API) so game code never references `bgfx::*` symbols. Landed in Sama (commits `ab3c9c5`, `b635de0`, `2fb051b`, `667ba75`); sample_game migrated. |

## Open bug reports

| # | Bug | Severity | Component | Summary |
|---|---|---|---|---|
| B1 | [SoLoud SIGSEGV on Android AAudio callback during init](bug-reports/soloud-android-crash.md) | crash on launch | `engine_audio` (SoLoud + miniaudio) | Race during `SoLoudAudioEngine::init` — AAudio callback thread fired before SoLoud's `mResampleData` was allocated. **Adopted** in sama `0e9bf2e` (proper fix: atomic gate in the SoLoud miniaudio callback shim, re-enabling AAudio rather than the interim `46b4ec1` OpenSL ES fallback). Verified on Pixel 9: no SIGSEGV. |
| B2 | [~20 ms/frame of unaccounted CPU on Android](bug-reports/android-cpu-time-regression.md) | perf (45-50 FPS vs expected 60) | `engine_rendering` — bgfx silently single-threaded on Android Vulkan | **Hypothesis 1 confirmed via vsync-off diagnostic**: `bgfx::frame` is still 15.7–23.6 ms with `BGFX_RESET_VSYNC` cleared. Hypothesis 2 (vsync queue stall) ruled out. bgfx Android Vulkan is silently running single-threaded despite `EngineDesc::singleThreaded = false`, `BGFX_CONFIG_MULTITHREADED=1` in the compile flags, and `Renderer::init` correctly skipping the pre-init `bgfx::renderFrame()` call. Engine-side investigation needed: confirm whether bgfx's render thread is actually being spawned on this device. |
| B3 | [Gyro/accel events never arrive on Android](bug-reports/android-gyro-no-events.md) | functional (no tilt input) | `engine_platform_android` — `AndroidGyro::init` | `AndroidGyro` creates its sensor event queue with `ident = ALOOPER_POLL_CALLBACK` and a null callback — invalid combo per NDK docs; the looper silently drops the queue. **Adopted** in sama `1f9abcf` (resolve resume-before-init race that left gyro silently disabled). Verified on Pixel 9: tilting the phone now rolls the ball as designed. |

## Status values

- **proposal** — written, not yet sent upstream.
- **filed** — submitted as an issue or PR on the Sama repo (link in the proposal's status header).
- **landed** — merged into Sama; the `sample_game`-side migration may still be pending.
- **adopted** — merged AND `sample_game` has migrated.
- **rejected** — discussed and declined; the proposal stays here as historical context with a brief reason.

## How to add a new proposal

1. Create `proposals/<short-kebab-name>.md`.
2. Use this front-matter block:
   ```markdown
   # Sama <Subsystem> — <Title>

   **Status:** proposal
   **Author:** sample_game
   **Targets:** <engine_subsystem(s)>
   ```
3. Cover, in order: Problem · Goals · Non-goals · Background · Proposal · API/ABI compatibility · Migration plan · Alternatives considered · Open questions · Estimated effort.
4. Add a row to the **Active proposals** table above with the proposal number, link, status, target, and a one-sentence summary.

The point is that each proposal stands alone — a Sama maintainer should be able to read just the linked file (without scrolling to other proposals) and have everything they need to evaluate it.

## How to add a new bug report

1. Create `bug-reports/<short-kebab-name>.md`.
2. Use this front-matter block:
   ```markdown
   # <Symptom>: <one-line title>

   **Severity:** <crash on launch | crash mid-game | wrong behaviour | perf regression>
   **Component:** <engine_subsystem>
   **Status:** open
   **First seen:** sama `<commit>`  (last known good: `<commit>`)
   **Reporter:** sample_game integration
   ```
3. Cover, in order: TL;DR · Environment · Repro · Logs · Backtrace · Suspected cause · What we ruled out · Suggested investigation direction · Workaround in `sample_game` · Acceptance test.
4. Add a row to the **Open bug reports** table above with severity, component, and a one-sentence summary.

## Why this lives here, not in Sama

`sample_game` is the closest real consumer of the engine, so problems and gaps tend to surface here first. Drafting proposals against the consumer code (with concrete before/after examples from this game) makes them grounded; drafting them inside Sama's own repo would risk hand-waving over what real callers actually need. Once a proposal is filed against Sama proper, this file's status field changes accordingly — the index is the single source of truth for what's been thought through and where.
