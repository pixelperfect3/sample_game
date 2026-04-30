# Sama Rendering — Close the bgfx Abstraction Boundary

**Status:** proposal
**Author:** sample_game
**Targets:** `engine_rendering` (`RenderPass`, `Renderer`, new `FrameStats` API)

---

## 1. Problem

Sama's stated abstraction is that *games consume engine APIs and never call bgfx directly*. `engine::rendering::RenderPass` already wraps view configuration (`rect`, `clearColorAndDepth`, `transform`, `framebuffer`, `touch`); `engine::ui::UiRenderer::render()` recently absorbed its own view setup as part of this push.

`sample_game` was audited for residual `bgfx::*` usage. **17 direct call sites remain**, all in `SampleGame.cpp`. None are *justified* — every one is a small gap in the engine API:

- 6× `bgfx::setViewName` to label the engine's own built-in views (Shadow / Opaque / Transparent / UI / ImGui), so they show up in the perf overlay and in GPU debuggers (RenderDoc, AGI, Instruments). The engine should be doing this itself.
- 4× `bgfx::setViewClear(NONE)` and `bgfx::setViewClear(COLOR, ...)` because `RenderPass` has only `clearColorAndDepth` and `clearDepth`. The "clear nothing" reset is needed because clear flags persist across frames in bgfx (this caused a real rendering bug earlier — see `SampleGame::onRender` Android-return path). The "color only" mode is needed because UI overlay views want to clear color without touching depth.
- 1× `bgfx::setViewName` + 1× `bgfx::setViewRect` + 1× `bgfx::touch` triple to configure the game's UI view. The "name" call is the only thing that forces `<bgfx/bgfx.h>` here; `RenderPass` already covers the other two.
- 1× `bgfx::setDebug(BGFX_DEBUG_PROFILER)` to enable per-view stats collection (perf overlay).
- ~10× field accesses on `bgfx::Stats` / `bgfx::ViewStats` (perf overlay) — `cpuTimerFreq`, `gpuTimerFreq`, `cpuTimeBegin/End`, `gpuTimeBegin/End`, `numDraw`, `numPrims`, `numViews`, `textureMemoryUsed`, `rtMemoryUsed`, plus per-view `name` / `cpuTimeBegin/End` / `gpuTimeBegin/End`.
- `bgfx::ViewId` typedef used 3× in helper signatures.

Knock-on effect: `<bgfx/bgfx.h>` is transitively pulled into game code via `engine/rendering/RenderPass.h`. A truly clean abstraction means the game should not be able to even *type* `bgfx::` and have it resolve.

## 2. Goals

1. Game code never references `bgfx::*` symbols or `BGFX_*` macros.
2. Game code never `#include`s `<bgfx/bgfx.h>` directly or transitively.
3. The wrappers stay zero-cost — same number of bgfx calls happen, just wrapped.
4. GPU debugger labels (view names) work everywhere with no per-game boilerplate.
5. Per-frame perf data (FPS, draws, primitives, per-pass CPU/GPU time, texture memory) is available to games as plain engine types — `float`, `unsigned`, `std::string_view`.

## 3. Non-goals

- **A new abstraction over bgfx.** Keep this thin — it's wrapping an existing API, not designing a new one.
- **Hiding bgfx from the engine itself.** `engine_rendering` continues to call bgfx; only the *boundary* with games is cleaned up.
- **Multi-backend portability.** bgfx already provides that; this proposal doesn't change the picture.

## 4. Background — current state

```cpp
// engine/rendering/RenderPass.h
class RenderPass
{
public:
    explicit RenderPass(bgfx::ViewId);
    RenderPass& framebuffer(bgfx::FrameBufferHandle);
    RenderPass& rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    RenderPass& clearColorAndDepth(uint32_t rgba, float depth = 1.f);
    RenderPass& clearDepth(float depth = 1.f);
    RenderPass& transform(const Mat4& view, const Mat4& proj);
    RenderPass& touch();
    bgfx::ViewId viewId() const;
};
```

`bgfx::ViewId` (== `uint16_t`) leaks through the constructor and `viewId()`. Clear modes don't include "color only" or "none". No `name()`. No way to read frame stats without going to `bgfx::getStats()`.

## 5. Proposal

### 5.1 Engine-side type alias

```cpp
// engine/rendering/ViewIds.h
namespace engine::rendering
{
    using ViewId = uint16_t;  // same underlying type as bgfx::ViewId
}
```

`RenderPass`'s constructor and `viewId()` shift to `engine::rendering::ViewId`. The bgfx aliasing is in one place; games never see the bgfx type.

### 5.2 Extend `RenderPass`

```cpp
class RenderPass
{
public:
    // ...existing...
    RenderPass& name(const char* label);    // wraps bgfx::setViewName
    RenderPass& clearColor(uint32_t rgba);  // BGFX_CLEAR_COLOR only
    RenderPass& clearNone();                // BGFX_CLEAR_NONE — resets persistent clear state
};
```

Three one-liners. `name()` is the high-leverage one — it matters for GPU debugger labelling and was the only reason `<bgfx/bgfx.h>` had to leak into `SampleGame.cpp`'s UI helpers.

`clearNone()` is critical: bgfx's `setViewClear` flags persist across frames. The game's "return to title" path observed an Android bug where stale `CLEAR_COLOR_AND_DEPTH` from a previous frame wiped the opaque pass. The fix needed to call `bgfx::setViewClear(view, BGFX_CLEAR_NONE)` directly. With `clearNone()` this becomes `RenderPass(view).clearNone()`.

### 5.3 Engine self-labels its built-in views

Add `Renderer::setupDefaultViewNames()`, called once during `Renderer::init` (after `bgfx::init`):

```cpp
void Renderer::setupDefaultViewNames()
{
    for (uint16_t i = 0; i < kMaxShadowViews; ++i)
    {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "Shadow %u", i);
        bgfx::setViewName(static_cast<bgfx::ViewId>(kViewShadowBase + i), buf);
    }
    bgfx::setViewName(kViewDepth,       "Depth Prepass");
    bgfx::setViewName(kViewOpaque,      "Opaque");
    bgfx::setViewName(kViewTransparent, "Transparent");
    bgfx::setViewName(kViewUi,          "UI 3D");
    bgfx::setViewName(kViewImGui,       "ImGui");
    // kViewGameUi (48) and kViewDebugHud (49) are owned by the game / DebugHud
    // respectively; UiRenderer::render and DebugHud::end already set those.
}
```

This deletes the 6-line block in `SampleGame::onInit` that does exactly this today.

### 5.4 New `FrameStats` API

```cpp
// engine/rendering/FrameStats.h
namespace engine::rendering
{
struct PassStats
{
    std::string_view name;
    float cpuMs;
    float gpuMs;
    bool  gpuValid;   // false if backend reports a wraparound / negative reading
};

struct FrameStats
{
    float    cpuMs;            // whole-frame CPU submit cost
    float    gpuMs;            // whole-frame GPU cost
    unsigned numDraw;
    unsigned numPrims;         // triangles
    unsigned textureMemoryMB;
    unsigned rtMemoryMB;

    // Per-view stats.  span lifetime: until the next sampleFrameStats() call.
    std::span<const PassStats> passes;
};

/// Enable bgfx's internal profiler.  Per-view stats are not collected
/// until this is called; cheap to call repeatedly (idempotent).
void enableGpuProfiler(bool on = true);

/// Sample the current frame's stats.  Cheap — wraps bgfx::getStats() and
/// converts timer ticks → milliseconds, bytes → megabytes.  Does NOT
/// allocate; the returned PassStats span aliases an internal buffer.
FrameStats sampleFrameStats();
}
```

Implementation is purely a wrapper; total ~50 lines. Hides `cpuTimerFreq`, `gpuTimerFreq`, `cpuTimeBegin/End`, `gpuTimeBegin/End`, `viewStats[i]`, etc.

The `gpuValid` flag formalizes the artifact-detection logic the game currently does inline (`gpuMs >= 0 && gpuMs < 100ms`). When a view's framebuffer is reconfigured mid-frame (e.g. `Renderer::beginFrameDirect()` swapping `kViewOpaque` between scene-FB and backbuffer), bgfx's begin/end timestamps come from inconsistent contexts and produce nonsense readings. Marking those as invalid at the engine level lets every consumer skip the manual filter.

### 5.5 Game-side example — perf overlay before/after

Before:
```cpp
const bgfx::Stats* s = bgfx::getStats();
const double cpuFreq = static_cast<double>(s->cpuTimerFreq);
const double gpuFreq = static_cast<double>(s->gpuTimerFreq);
const double frameCpuMs = 1000.0 * (s->cpuTimeEnd - s->cpuTimeBegin) / cpuFreq;
const double frameGpuMs = 1000.0 * (s->gpuTimeEnd - s->gpuTimeBegin) / gpuFreq;
for (uint16_t i = 0; i < s->numViews; ++i) {
    const bgfx::ViewStats& v = s->viewStats[i];
    if (v.name[0] == '\0') continue;
    const double passCpuMs = 1000.0 * (v.cpuTimeEnd - v.cpuTimeBegin) / cpuFreq;
    const double passGpuMs = 1000.0 * (v.gpuTimeEnd - v.gpuTimeBegin) / gpuFreq;
    const bool   gpuValid  = passGpuMs >= 0.0 && passGpuMs < 100.0;
    // ...print...
}
```

After:
```cpp
const auto fs = engine::rendering::sampleFrameStats();
print("CPU %5.2f ms   GPU %5.2f ms   Draws %4u   Prims %5u",
      fs.cpuMs, fs.gpuMs, fs.numDraw, fs.numPrims);
for (const auto& p : fs.passes)
    print("%-18s %8.2f %s", p.name, p.cpuMs,
          p.gpuValid ? format("%.2f", p.gpuMs) : "--");
```

No `bgfx::*`, no manual unit conversion, no manual artifact filter, no `<bgfx/bgfx.h>` include.

## 6. API/ABI compatibility

- `RenderPass` constructor signature changes from `bgfx::ViewId` to `engine::rendering::ViewId` — but they're the same underlying type (`uint16_t`), so callers compile unchanged.
- `RenderPass::viewId()` return type changes likewise.
- New methods on `RenderPass` (`name`, `clearColor`, `clearNone`) are pure additions.
- `FrameStats` and `enableGpuProfiler` are new headers; nothing existing to break.
- Existing direct `bgfx::setViewName` calls in game code still compile (bgfx headers remain accessible). They become *unnecessary* after the engine-side default naming lands, but we don't break them.

## 7. Migration plan for `sample_game`

1. Replace `static void setupUiView(bgfx::ViewId, ...)` with inline `RenderPass(view).name("…").rect(...).clearNone().touch()` chains. Delete the local helper (`SampleGame.cpp:149-155`).
2. Delete the 6-line `bgfx::setViewName` block in `SampleGame::onInit` for the engine views (`SampleGame.cpp:379-386`) — engine handles it now.
3. Replace the title-screen `setViewRect` + `setViewClear(COLOR, 0)` + `touch` triple with `RenderPass(kViewGameUi).rect(...).clearColor(0x00000000).touch()` (`SampleGame.cpp:1255-1259`).
4. Replace the defensive `bgfx::setViewClear(kViewTransparent, BGFX_CLEAR_NONE)` with `RenderPass(kViewTransparent).clearNone()` (`SampleGame.cpp:1342`).
5. Rewrite `renderPerfOverlay` against `engine::rendering::sampleFrameStats()` (`SampleGame.cpp:1820-1920`) — biggest LOC reduction by far.
6. Drop `<bgfx/bgfx.h>` from any transitive path the game owns. `<bgfx/bgfx.h>` should now be invisible to game code (verifiable with `grep -r 'bgfx::' SampleGame.cpp` returning nothing).

## 8. Alternatives considered

### 8.1 Keep games calling bgfx directly for "advanced" cases

Already rejected by Sama's stated abstraction policy in `docs/NOTES.md`. The audit found zero genuinely-advanced uses; every direct call is filling a small wrapper gap.

### 8.2 Build the perf-overlay logic *into* Sama

Could ship a built-in `engine::rendering::PerfOverlay` widget that sample games just toggle. **Deferred.** The current per-game overlay has game-specific sections (entity count, game-CPU breakdown, level-specific notes) that don't belong in the engine. The right scope here is: engine exposes the data; games pick the presentation.

### 8.3 `RenderPass::clearNone` should auto-fire on every frame

Tempting (it'd remove a class of "stale persistent state" bugs entirely), but it would break legitimate use cases that *want* the same clear settings frame after frame. Explicit reset is right.

## 9. Open questions

1. Should `setupDefaultViewNames` also run on `Engine::initAndroid`? Yes — call site is `Renderer::init` regardless of platform.
2. `FrameStats::passes` lifetime — span-into-internal-buffer is fine for synchronous reads inside one frame. If we ever want async / off-thread consumers, switch to a small inline `std::array<PassStats, 16>` and copy.
3. Should `enableGpuProfiler(false)` ever be useful at runtime? Maybe for shipping builds — bgfx's internal profiler has measurable overhead on some backends. Gate the calls behind a debug flag if so.

## 10. Estimated effort

- `RenderPass` extensions: ~15 lines.
- `Renderer::setupDefaultViewNames`: ~15 lines, called once.
- `FrameStats.h` / `.cpp`: ~80 lines including unit conversion + artifact detection.
- `engine::rendering::ViewId` typedef: 3 lines.
- Tests (Catch2): a smoke test that `sampleFrameStats` returns sane numbers in a render-loop test fixture: ~40 lines.
- Docs: AGENTS.md cheat sheet snippet showing the new pattern: ~20 lines.

Total: ~170 lines of engine code, ~90 minutes of focused work. Game-side cleanup is another ~50-line diff that *deletes* code.
