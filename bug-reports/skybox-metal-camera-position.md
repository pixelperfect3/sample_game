# SkyboxRenderer renders uniform single-face color on Metal (macOS)

**Severity:** wrong behaviour (skybox unusable on Metal)
**Component:** `engine_rendering` — `engine/shaders/vs_skybox.sc`
**Status:** **fix written, ready to upstream** (patch: `patches/sama_skybox_metal_camera_pos_fix.patch`)
**First seen:** sama `dfd4162` (when `SkyboxRenderer` + `vs_skybox.sc` landed)
**Reporter:** `sample_game` integration (verified on macOS 14, M-series, Metal backend)

---

## TL;DR

`SkyboxRenderer::render` on macOS / Metal paints the entire viewport with whichever cube face the camera is pointing at — the skybox is sampled with a single constant direction across every fragment. The vertex shader's per-vertex direction degenerates to "camera-forward" because `camPos` lands at `(0, 0, 0)` instead of the actual camera world position. Caused by a spirv-cross transcription quirk in the matrix-element access path: `u_invView[3][0..2]` in the GLSL source becomes `u_invView[0..2][3]` in the generated MSL, which reads the matrix's last row (all-zero for an affine view) instead of its last column (the translation). The same shader works correctly under GLSL and SPIRV (Vulkan/Android) — only the Metal backend is affected.

## Reproduce

1. Build `sample_game` on macOS (Metal backend).
2. Set `kDebugStartLevel = 0` so a level loads on launch (default `-1` shows the title screen with no skybox).
3. Run the app and observe the viewport background.

Expected: park cubemap (CC0 Museumplein) visible behind the plank — grass below, sky above, trees at horizon.
Actual: viewport background is a single uniform colour matching the cube face along the camera's forward axis (chase camera looks roughly +X → entire viewport renders the +X face).

The bug is also visible with the procedural sky (`IblResources::generateDefault`) and a test cubemap whose faces are pure (R, G, B, C, M, Y); the screen renders whichever face dominates the camera's forward direction, with no gradient.

## What we ruled out

Captured these signals before locating the cross-compile bug:

- **Cubemap file format** — `loadCubemapEnvironment` returns the EnvironmentAsset successfully, and `prefilteredFaces[f][0]` centres dumped through `fprintf` show distinct per-face values (e.g. +Y is `(0.49, 0.58, 0.75)` for the park cube — actual sky blue).
- **`IblResources::upload`** — returns true; `brdfLut_` is valid; the cubemap GPU handle is non-zero.
- **bgfx face order** — the prefilter integration uses `cubeUvToDir(f, u, v)` to drive `sky(N)`, which matches the OpenGL face convention. Verified by loading `sama/assets/env/test_cubemap.ktx` (the engine's known-good R/G/B/C/M/Y test cube) and confirming each face's center pixel arrives with the expected colour at upload.
- **Mip selection** — the same uniform-face symptom occurs with `BGFX_SAMPLER_{MIN,MAG,MIP}_POINT | UVW_CLAMP` forced at bind time, so the sampler isn't picking mip 7 by accident. The bug is in the direction vector itself, not in the sample LOD.
- **Bloom / tonemap** — turning bloom off and disabling auto-exposure produced the same single-colour result; the post-process chain isn't washing the cube out.

## Root cause

`vs_skybox.sc:34` reads the camera translation column as element accesses:

```glsl
vec3 camPos = vec3(u_invView[3][0], u_invView[3][1], u_invView[3][2]);
```

In GLSL `u_invView[3]` is the 4th column (column-major), and `[i][j]` is `[col][row]`, so this evaluates to `(Tx, Ty, Tz)` — the camera world position. The GLSL output of bgfx-shaderc preserves this correctly (`u_invView[3].x`, `.y`, `.z`).

The MSL output, however, transcribes it as:

```metal
float3(_mtl_u.u_invView[0u][3], _mtl_u.u_invView[1u][3], _mtl_u.u_invView[2u][3])
```

MSL matrices are also column-major and indexed `[col][row]`, so `u_invView[0..2][3]` reads `(M[0][3], M[1][3], M[2][3])` — the bottom row's first three elements. For an affine view matrix that row is `(0, 0, 0)`, so `camPos` is `vec3(0)` and:

```glsl
v_worldPos = nearWorld - camPos  →  nearWorld
```

`nearWorld` is `camera_world_pos + (perpendicular_offset × near_distance)`. The perpendicular offset is ~`tan(fov/2) × near` = on the order of 0.02–0.04 units; the camera position itself is several units long. After `normalize`, the result is dominated by the camera position vector — so every fragment receives **the same direction**, and every fragment samples the same cube face.

Confirmation: replacing `gl_FragColor = textureCube(s_skybox, dir)` with `gl_FragColor = vec4((dir + 1.0) * 0.5, 1.0)` paints the viewport a single solid colour (uniform direction). With `camPos` fixed (see below), the same diagnostic paints a proper screen-space colour gradient (per-pixel direction varies).

## Fix

Replace element-access with a matrix–vector multiply that transforms the origin to world space — semantically identical, but spirv-cross emits it correctly across all backends:

```glsl
vec3 camPos = mul(u_invView, vec4(0.0, 0.0, 0.0, 1.0)).xyz;
```

Diff in `patches/sama_skybox_metal_camera_pos_fix.patch`.

After the fix, the park cubemap renders the actual park scene at level 0 and (presumably — not yet verified on Android) the space cubemap at level 1.

## Acceptance test

1. Load `sample_game` on macOS / Metal at level 0.
2. The plank ball-roll scene should sit on a grass-coloured ground with sky and trees visible in the background, taken from `assets/textures/skyboxes/park.ktx`.
3. Sample any background pixel — the colour should change as the camera moves, not remain a constant face hit.

## Notes for the upstream PR

- The same `u_invView[i][j]` pattern doesn't appear elsewhere in `engine/shaders/`; this is the only site to fix.
- Worth a one-line entry in `docs/NOTES.md` (something like "bgfx spirv-cross transcribes `u_invView[3][0]` as `u_invView[0][3]` on the Metal backend — prefer `mul(M, vec4(0,0,0,1)).xyz` over column element-access when extracting translation").
- Independent of platform: the same change keeps the GLSL/SPIRV/ESSL paths byte-identical (after compile) — only the MSL output differs.
