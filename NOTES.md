# Change notes

A running log of changes made to the game. The authoritative history is the
git log — this file summarises the "why" behind notable shifts and the
design trade-offs considered at each step.

## Initial build

Plank-and-coin demo on the Sama engine. A grey plank suspended at y=2 with a
red ball (dynamic, sphere collider) at one end and a yellow coin at the
other. Tilting the plank with arrow keys rolled the ball toward the coin;
on contact a beep played and the coin was destroyed.

- `IGame` + `GameRunner` for the frame loop.
- Jolt physics via `PhysicsSystem` + contact events.
- SoLoud audio (`beep.wav` procedurally generated).
- Follow camera using `OrbitCamera` at first, later replaced.

## Coin as a sensor

The ball's first contact with the coin applied a small impulse before the
coin was destroyed, so the ball got a tiny bump. Tried two fixes:

1. **Manual distance check (no collider on the coin)** — simple, but
   bypasses the physics engine's broadphase and doesn't extend to multiple
   pickups.
2. **Sensor flag on the coin's collider** — the right abstraction, but
   Sama's `ColliderComponent` didn't expose `isSensor` yet.

Resolved by pulling the latest Sama (commit `5695e7e`) which added
`ColliderComponent::isSensor`. The coin now participates in collision
detection without collision response — ball passes through, contact event
still fires.

## glTF assets for ball and coin

Replaced the placeholder cubes with procedurally-generated GLBs:

- `tools/make_primitives.py` builds `assets/models/sphere.glb` (24×32 UV
  sphere, diameter 1.0) and `assets/models/coin.glb` (32-slice capped
  cylinder, diameter 1.0, thickness 0.1). Hand-rolled GLB binary writer, no
  Python deps.
- Entities are created in `onInit` with the cube mesh as a placeholder;
  `applyLoadedAssets()` swaps in the GLB meshes once `AssetManager::state()`
  reports Ready. One-frame flash of cubes on first load.
- Diameters chosen to match the engine's unit-cube convention (size 1.0) so
  entity `scale` doesn't have to change when swapping in the GLB.

**Abstraction note:** The engine's intended entry point is
`GltfSceneSpawner::spawn()`, which creates entities for you. But it doesn't
return entity IDs and can't attach physics components to the entities it
creates, so we harvest `asset->meshes[0]` / `asset->materials[0]` manually
and `RenderResources::addMesh()` them ourselves. Abstraction gap in Sama.

## Follow camera

Replaced `OrbitCamera` (unused after this point) with a chase camera:
position behind and above the ball, looking at the coin. The camera tracks
the ball's live position each frame.

**Camera iterations:**

1. First version pointed the camera at the ball → coin appeared "past" the
   ball in the frame. Worked but was really a 3rd-person cam, not a
   "looking at the coin" cam.
2. Refactored to look at the coin position directly. When the ball got
   near the coin, the camera overshoot the coin and pointed down — felt
   bad.
3. Added a min-distance clamp so the camera anchor stopped 3 units behind
   the coin. Looked clean for the approach but felt static.
4. Removed the clamp on request — camera now tracks the ball at any
   distance. Impact is visible from right behind the ball.
5. After the coin is collected, camera freezes at the fixed min-distance
   spot using the initial ball→coin direction.

## Force-based movement

Dropped plank tilting in favour of direct ball control.
`physics_.applyForce()` in `onFixedUpdate` pushes the ball along world X/Z
while movement keys are held. Holding a direction accumulates speed.

- Force magnitude 45 N.
- W/Up = +X (toward coin), S/Down = −X, A/Left = −Z, D/Right = +Z.
  Swapped L/R once to match the follow-camera orientation.
- Plank is now static (kept as a visible platform).
- Removed the now-unused `plankRoll_` member.

## Ball weight tuning

Mass 3 → 6 → 10 → 30 → 100 → 20. Terminal velocity under constant force is inversely
proportional to mass with Jolt's velocity-proportional damping
(v_term = F/(m·D)), so heavier ball = lower top speed **and** lower
acceleration. Chose mass over damping for the "weighty" feel: slow to get
going, slow to stop, coasts after release.

## Coin visual

- Stood upright with a 90° X-axis rotation (cylinder axis along X → flat
  faces pointing along ±X).
- Added a 90° Y rotation on top for nicer initial orientation.
- Continuous Y-axis spin at 180°/s rebuilt each frame in `onUpdate` from
  accumulated `coinSpinTime_`.

## Figure-8 level (ring version)

Replaced the plank floor with a **figure-8 (number "8") ring floor**.

- Two rings (annuli) — outer R=4, inner R=2.5, centres at x=±3.5. Rings
  overlap slightly at x=0 so the ball can cross between them.
- Built as a 0.4-unit grid of box tiles filling both annuli. Hollow
  centres of each ring = gaps. Ball falls through if it enters the holes.
- Thickness 1.0 (top at y=0). Ball starts at the leftmost edge of the
  left ring.
- Invisible safety collider at y=−20 catches the ball if it falls.
- Coin spawns at a random (θ, r) on the **right ring** (annulus
  sampling). R reset re-rolls it and keeps the ball resetting too.

Chase camera tracks the ball and looks at the stored `coinSpawnPos_`.
When the coin is collected, camera freezes using `ballPosAtCollection_`
snapshotted at the moment of contact, so `fwd` doesn't drift while the
ball keeps moving afterwards.

Prior "tiled disk" and "walled ring" attempts were discarded — user
wanted the floor itself to be an "8" glyph, i.e. two rings, not filled
circles and not walls.

Mesh colliders still aren't implemented in Sama (falls back to Box), so
tiled boxes remain the workaround.

## Three coins, ball mass 15

Placed three coins at random positions around the figure-8:
- Coin 0 always spawns on the right ring (ensures at least one target
  ahead of the ball's starting edge).
- Coins 1 and 2 pick a ring randomly (50/50) and a random (θ, r) within
  that ring's annulus.

Collision handler loops over the coin array, plays the beep per hit,
destroys the hit coin. `coinsRemaining_` counts down; camera targets
the nearest uncollected coin each frame; once all are collected, camera
freezes at the last one using `ballPosAtCollection_`.

Ball mass 20 → 15 (acceleration at 45 N is now 3.0 m/s², a bit snappier).

## Instancing (deferred)

Sama has `InstancedMeshComponent` + `InstanceBufferBuildSystem`, but
**no instanced PBR vertex shader** is exposed on `Engine`. The existing
`pbrProgram` reads transforms from `bgfx::setTransform` uniforms, not
from the instance data buffer. Using it with `InstanceBufferBuildSystem`
would render all instances at the origin.

A proper instancing path needs an engine change: an instanced vertex
shader that pulls the world matrix from instance attributes, compiled
via Sama's `engine_shaders`, and exposed on Engine as
`instancedPbrProgram()`. At 3 coins the per-frame savings are zero, so
we stayed with per-entity draw calls.

## Bloom (deferred)

Sama has a full post-process system (SSAO / bloom / tonemap / FXAA) but the
demo apps all use `renderer.beginFrameDirect()` which bypasses it. Enabling
bloom here would require switching to `renderer.beginFrame()` + calling
`postProcess().submit()` at the end of each frame, plus verifying the PBR
shader's tonemapping path still looks right. Not yet done.

## Transparent ball (deferred)

`DrawCallBuildSystem::update()` submits all entities with
`BGFX_STATE_DEFAULT` — no alpha blending, no back-to-front sort, no
separate transparent pass. Setting `albedo.w < 1.0` has no visual effect
today. Proper transparency would need a `TransparentTag` component + a new
`submitTransparent()` method on the draw-call system. ~50-line Sama change.

## UI migration to engine::ui + MSDF (2026-04)

### What changed

Ripped out ImGui for the in-game HUD and title screen. All game UI now
goes through Sama's `engine::ui` system:

- Title screen (Sample Game title, Start Game button, objective text,
  Controls list) — retained-mode `UiCanvas` with `UiText` + `UiButton`
  + `UiPanel` widgets.
- Level-complete / You-Win screen + Next Level button — retained-mode
  `UiCanvas` built lazily when `coinsRemaining_` crosses to 0 and
  torn down when the next level loads.
- Coin counter ("Coins Collected: N/M") — per-frame `UiDrawList`
  `drawText` on the `kViewGameUi` bgfx view (immediate-mode is fine
  since the string changes every frame anyway).

All text rendering uses `engine::ui::MsdfFont` loaded from
`assets/fonts/JetBrainsMono-msdf.{json,png}` (copied out of the Sama
repo under `build/_deps/sama-src/assets/fonts/`). CMake gained
`engine_ui` alongside `sama_3d` in the `target_link_libraries` line.

### Why

ImGui was being used at `SetWindowFontScale(10.0f)` / `12.0f` to get
HUD-sized text, which upscales the 13-px bitmap font with nearest-
neighbor filtering and looks pixelated at 1080p and awful at 4K.
MSDF gives sharp edges at any draw size from one atlas, which is
exactly the same reason Sama's editor uses it for its status overlay.
Switching also lets the title screen use real styled buttons with
hover/pressed states instead of ImGui's default look, and pulls the
in-game UI into the same rendering path the engine already uses for
everything else (one bgfx view, no separate ImGui context to manage).

### Architecture

`SampleGame` now owns:

- `engine::ui::MsdfFont hudFont_` — loaded once in `onInit`, shutdown
  in `onShutdown`.
- `engine::ui::UiRenderer uiRenderer_` — init/shutdown alongside the
  font; shared by every UI view this frame.
- `engine::ui::UiDrawList hudDrawList_` — reused per frame for the
  coin-counter text (cleared at the start of the draw, filled with a
  single `drawText` call, submitted through `uiRenderer_.render`).
- `std::unique_ptr<UiCanvas> titleCanvas_` — built once at first
  render via `buildTitleCanvas()`, rebuilt on framebuffer resize.
- `std::unique_ptr<UiCanvas> endLevelCanvas_` — built lazily by
  `buildEndLevelCanvas(hasNextLevel)` when the player completes a
  level, rebuilt if the `hasNextLevel` bit flips (final level → YOU
  WIN, no button).

Canvases are rendered on `engine::rendering::kViewGameUi` (view id 48,
reserved for in-game HUDs) with `BGFX_CLEAR_NONE` so they composite
over the 3D scene. Mouse events come from `engine.inputState()`
multiplied by `engine.contentScaleX()/Y()` (logical → framebuffer
pixels), synthesized into `UiEvent`s and dispatched to whichever
canvas is active that frame.

Font-load failure is handled by a `hudFontLoaded_` bool; the HUD
silently draws nothing if loading fails (a warning is logged to
stderr). Buttons are unreachable without the font, which is the same
failure mode the editor's HUD overlay already has.

### Gotchas

- `UiCanvas` ctor takes `(width, height)` by value but there is no
  default constructor, so the members are held as `std::unique_ptr`
  and built on the first frame once we know `engine.fbWidth()`. Any
  future framebuffer resize rebuilds `titleCanvas_` by simply
  reconstructing it in place — the builder functions are idempotent.
- `engine::input::InputState::mouseX()` is in **logical window
  pixels**, not framebuffer pixels. On a retina display you must
  multiply by `engine.contentScaleX()/Y()` before sending the
  coordinate to `canvas.dispatchEvent`, otherwise hit-testing is off
  by 2x on Mac. `apps/ui_test/UiTestApp.cpp::dispatchMouseEvents`
  does the same thing.
- `UiText` widgets measure and lay out their own rects; font size
  and anchor+offset pair must be set before the first `update()`
  or nothing shows up. Setting `font = &hudFont_` on each widget is
  required even though the canvas has no global font pointer.
- `math::Vec*` lives in `engine::math`, not the global `math`
  namespace used by some glm-heavy code in this project. I first
  tried `math::Vec4{...}` and got "undeclared identifier 'math'"
  before qualifying.
