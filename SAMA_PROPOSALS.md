# Sama Engine Proposals

Design proposals for changes to the [Sama engine](https://github.com/pixelperfect3/sama) that originated from `sample_game`. Each entry is a self-contained design doc — problem statement, API, implementation sketch, alternatives, migration plan — written so it can be sent upstream as a PR description, GitHub issue body, or RFC without further editing.

This file is the **index**. Individual proposals live as separate files in `proposals/`.

---

## Active proposals

| # | Proposal | Status | Targets | Summary |
|---|---|---|---|---|
| 1 | [Compound & Mesh Collider Shapes](proposals/physics-shapes.md) | proposal | `engine_physics` | Allow one rigid body to represent N convex children (`StaticCompoundShape`) or a triangle soup (`MeshShape`), referenced by shape ID. Eliminates the N-entities-for-one-piece-of-geometry pattern (e.g. figure-8 floor: 128 colliders → 1). |
| 2 | [Close the bgfx Abstraction Boundary](proposals/bgfx-abstraction.md) | proposal | `engine_rendering` | Three small additions (`RenderPass::name/clearColor/clearNone`, engine-side default view naming, new `FrameStats` API) so game code never references `bgfx::*` symbols. |

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
4. Add a row to the table above with the proposal number, link, status, target, and a one-sentence summary.

The point is that each proposal stands alone — a Sama maintainer should be able to read just the linked file (without scrolling to other proposals) and have everything they need to evaluate it.

## Why this lives here, not in Sama

`sample_game` is the closest real consumer of the engine, so problems and gaps tend to surface here first. Drafting proposals against the consumer code (with concrete before/after examples from this game) makes them grounded; drafting them inside Sama's own repo would risk hand-waving over what real callers actually need. Once a proposal is filed against Sama proper, this file's status field changes accordingly — the index is the single source of truth for what's been thought through and where.
