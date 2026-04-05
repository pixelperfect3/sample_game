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

Mass 3 → 6 → 10. Terminal velocity under constant force is inversely
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
