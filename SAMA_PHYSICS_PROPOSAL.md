# Sama Physics — Compound & Mesh Collider Shapes

**Status:** proposal
**Author:** sample_game
**Targets:** `engine_physics` (`IPhysicsEngine`, `JoltPhysicsEngine`, `PhysicsComponents`, `PhysicsSystem`)

---

## 1. Problem

A game's static level geometry — floors, walls, ramps, terrain, prop pieces — frequently consists of dozens to thousands of small convex pieces that share a single rigid body's physical identity. Sama currently forces one *Jolt body per ECS entity*, with one of four primitive shapes: `Box | Sphere | Capsule | Mesh` (and `Mesh` is unimplemented — it falls back to `Box`).

Concrete instance: `sample_game`'s figure-8 level. The floor is a tessellated annular ring sampled at `64 angular × 1 radial × 2 lobes = 128` boxes. Each box is a separate ECS entity with its own `RigidBodyComponent + ColliderComponent`, generating a separate Jolt body. The visual representation has been merged to one mesh (one draw call), so all 128 entities exist *only* to provide collision.

The cost is twofold:

- **Per-entity:** 128 entities in the registry that contribute nothing to gameplay logic; iterated by every system that views `RigidBodyComponent`.
- **Per-body:** 128 bodies in Jolt's broadphase. Once the bodies are `Static` (this proposal assumes that change is in place — and `sample_game` already made it), the per-frame cost is small but non-zero, and broadphase queries still pay for tree traversal across all leaves.

The natural representation is *one body, many shapes*. Jolt supports this directly via `StaticCompoundShape` (rigid composition of N child shapes) and `MeshShape` (acceleration structure over a triangle soup). Sama exposes neither.

## 2. Goals

1. Allow a single `RigidBodyComponent + ColliderComponent` pair to represent collision geometry composed of either:
   - **N convex children** (boxes / spheres / capsules) — `StaticCompoundShape`.
   - **A triangle soup** — `MeshShape` (static only — Jolt restriction).
2. Preserve the existing one-component-one-body ECS pattern for the simple case. No required changes for games using only primitives today.
3. Keep `ColliderComponent` POD and fixed-size — no variable-length data inside the component.
4. Allow shape data to be shared across multiple bodies (instancing — N pillar bodies referencing one compound shape).

## 3. Non-goals

- **Dynamic mesh bodies.** Jolt only supports `MeshShape` for static / sensor / kinematic-non-collider use. Matches Jolt's constraint; out of scope to fight it.
- **Convex hull from arbitrary point cloud.** A future `ColliderShape::ConvexHull` could fit the same pattern but is not part of this proposal.
- **Serialization round-trip through `SceneSerializer`.** Compound and mesh shapes are typically built at level-load time from procedural data. JSON round-trip is a separate problem.
- **Editor support.** Out of scope.

## 4. Background — current API

```cpp
// engine/physics/PhysicsComponents.h
enum class ColliderShape : uint8_t
{
    Box,
    Sphere,
    Capsule,
    Mesh   // documented as "triangle mesh, static only" — currently falls back to Box
};

struct ColliderComponent
{
    math::Vec3 offset;        // collider local offset from entity origin
    math::Vec3 halfExtents;   // Box
    float      radius;        // Sphere / Capsule
    ColliderShape shape;
    uint8_t    isSensor;
};  // 32 bytes, fixed
```

`JoltPhysicsEngine::addBody(BodyDesc)` reads these fields, builds a single `JPH::Shape`, and creates a `JPH::Body`. `BodyDesc` mirrors the component shape fields verbatim.

## 5. Proposal

### 5.1 New shape kinds

```cpp
enum class ColliderShape : uint8_t
{
    Box,
    Sphere,
    Capsule,
    Mesh,       // backed by a pre-built MeshShape, referenced by shapeID
    Compound    // backed by a pre-built StaticCompoundShape, referenced by shapeID
};
```

`Mesh` is now actually implemented; `Compound` is new.

### 5.2 `ColliderComponent` extension

Add one field — the rest stays untouched:

```cpp
struct ColliderComponent
{
    math::Vec3 offset;
    math::Vec3 halfExtents;
    float      radius;
    ColliderShape shape;
    uint8_t    isSensor;
    uint8_t    _pad[2];
    uint32_t   shapeID = ~0u;  // for Mesh / Compound; ignored for primitives
};  // grows from 32 → 36 bytes
```

For `Box | Sphere | Capsule`: `shapeID` is unused. Existing code continues to work bit-for-bit.

For `Mesh | Compound`: `shapeID` references a shape pre-built via the new `IPhysicsEngine` calls below. `offset`, `halfExtents`, `radius` are ignored.

### 5.3 `IPhysicsEngine` additions

```cpp
class IPhysicsEngine
{
    // ...existing API...

    // ---- Pre-built shapes (shareable across bodies) ------------------

    /// Build a static triangle-mesh shape.  Vertices are flat xyz floats;
    /// indices are 16- or 32-bit triangle list (CCW).  Returns ~0u on
    /// failure (degenerate input, allocation, etc.).
    ///
    /// Lifetime: the shape is reference-counted internally; it lives as
    /// long as any body references it AND `destroyMeshShape` has not
    /// been called.  Bodies retain their reference until removed.
    virtual uint32_t createMeshShape(const float* positions, size_t vertexCount,
                                     const uint32_t* indices, size_t indexCount) = 0;

    /// Decrement the engine's hold on a mesh shape ID.  Bodies that
    /// already reference it keep working; the underlying JPH::Shape is
    /// freed when the last reference drops.
    virtual void destroyMeshShape(uint32_t shapeID) = 0;

    /// Description of one child inside a compound.  Mirrors a subset of
    /// ColliderComponent fields, plus a local pose.  Compound children
    /// must be convex (Box / Sphere / Capsule).  Nesting is not allowed.
    struct CompoundChild
    {
        ColliderShape shape;            // Box | Sphere | Capsule
        math::Vec3    localPosition;    // relative to compound origin
        math::Quat    localRotation;
        math::Vec3    halfExtents;      // Box
        float         radius;           // Sphere / Capsule
        float         halfHeight;       // Capsule cylindrical part
    };

    /// Build a static compound shape from N convex children.  N up to
    /// ~10000 is fine; Jolt builds an internal AABB tree.
    virtual uint32_t createCompoundShape(const CompoundChild* children,
                                         size_t count) = 0;

    virtual void destroyCompoundShape(uint32_t shapeID) = 0;
};
```

### 5.4 Game-side example — figure-8 floor

Before (this proposal not yet landed):

```cpp
// 128 entities, 128 bodies
for (each angular segment * 2 lobes) {
    EntityID e = registry.createEntity();
    registry.emplace<TransformComponent>(e, ...);
    registry.emplace<RigidBodyComponent>(e, BodyType::Static);
    registry.emplace<ColliderComponent>(e, ColliderShape::Box, halfExtents);
}
```

After:

```cpp
// 1 entity, 1 body, 128 child shapes
std::vector<IPhysicsEngine::CompoundChild> children;
for (each angular segment * 2 lobes) {
    children.push_back({
        .shape         = ColliderShape::Box,
        .localPosition = wedgePos,           // local to figure-8 origin
        .localRotation = wedgeRot,
        .halfExtents   = wedgeHalfExtents,
    });
}
uint32_t shapeID = physics.createCompoundShape(children.data(), children.size());

EntityID floor = registry.createEntity();
registry.emplace<TransformComponent>(floor, /*at origin*/);
RigidBodyComponent rb; rb.type = BodyType::Static; rb.mass = 0;
registry.emplace<RigidBodyComponent>(floor, rb);
ColliderComponent c;
c.shape   = ColliderShape::Compound;
c.shapeID = shapeID;
registry.emplace<ColliderComponent>(floor, c);
```

For full mesh collision (e.g. a sculpted terrain), the same pattern with `createMeshShape` + `ColliderShape::Mesh`.

### 5.5 Implementation — `JoltPhysicsEngine`

#### Shape registry

Add a small registry inside `JoltPhysicsEngine`:

```cpp
struct ShapeEntry { JPH::ShapeRefC shape; };
std::unordered_map<uint32_t, ShapeEntry> meshShapes_;
std::unordered_map<uint32_t, ShapeEntry> compoundShapes_;
uint32_t nextShapeID_ = 1;
```

`JPH::ShapeRefC` is Jolt's intrusive-refcount handle. Once a body holds a reference, the underlying shape outlives the registry entry. So `destroyMeshShape` just erases the registry entry; Jolt's refcount keeps the shape alive while bodies still use it.

#### `createMeshShape`

```cpp
JPH::TriangleList tris;
tris.reserve(indexCount / 3);
for (size_t i = 0; i < indexCount; i += 3)
{
    const uint32_t i0 = indices[i+0], i1 = indices[i+1], i2 = indices[i+2];
    tris.emplace_back(toFloat3(positions, i0),
                      toFloat3(positions, i1),
                      toFloat3(positions, i2));
}
JPH::MeshShapeSettings settings(std::move(tris));
auto result = settings.Create();
if (result.HasError()) return ~0u;
const uint32_t id = nextShapeID_++;
meshShapes_.emplace(id, ShapeEntry{result.Get()});
return id;
```

#### `createCompoundShape`

```cpp
JPH::StaticCompoundShapeSettings settings;
for (size_t i = 0; i < count; ++i)
{
    JPH::ShapeRefC child = buildChildShape(children[i]);  // Box/Sphere/Capsule
    settings.AddShape(toJoltVec(children[i].localPosition),
                      toJoltQuat(children[i].localRotation),
                      child);
}
auto result = settings.Create();
if (result.HasError()) return ~0u;
const uint32_t id = nextShapeID_++;
compoundShapes_.emplace(id, ShapeEntry{result.Get()});
return id;
```

#### `addBody` — switch on shape

```cpp
JPH::ShapeRefC shape;
switch (desc.shape)
{
    case ColliderShape::Box: /* existing */ break;
    case ColliderShape::Sphere: /* existing */ break;
    case ColliderShape::Capsule: /* existing */ break;
    case ColliderShape::Mesh: {
        auto it = meshShapes_.find(desc.shapeID);
        if (it == meshShapes_.end()) return ~0u;
        shape = it->second.shape;
        break;
    }
    case ColliderShape::Compound: {
        auto it = compoundShapes_.find(desc.shapeID);
        if (it == compoundShapes_.end()) return ~0u;
        shape = it->second.shape;
        break;
    }
}
// rest of body creation unchanged
```

#### `BodyDesc` mirrors the component change

Add `uint32_t shapeID = ~0u;` to `BodyDesc`. `PhysicsSystem::registerNewBodies` copies it across like the other primitive params.

### 5.6 `PhysicsSystem` — no behavior change

`registerNewBodies`, `cleanupDestroyedBodies` already operate on bodies generically. `syncKinematicBodies` / `syncDynamicBodies` only iterate `Kinematic` / `Dynamic` bodies — `Static` compound/mesh bodies skip both naturally.

### 5.7 Lifetime / ownership notes

- A shape ID is owned by the engine until **both** of: (a) `destroy*Shape` is called, **and** (b) all bodies referencing it are removed. Whichever event fires last triggers actual deallocation.
- A typical game pattern: `createCompoundShape` at level-load, `addBody` referencing it, then on level-unload `removeBody` followed by `destroyCompoundShape`. The order of those two unload calls does not matter.
- `shutdown()` walks the shape registry and drops references, matching the existing `destroyAllBodies` behavior.

## 6. API/ABI compatibility

- `ColliderComponent` grows from 32 → 36 bytes. The `static_assert(sizeof(ColliderComponent) == 32)` in `PhysicsComponents.h` updates to 36. Any caller computing `sizeof` for a custom buffer needs a recompile, but no source change.
- `BodyDesc` gains a field; no behavior change for callers using only primitives.
- `IPhysicsEngine` gains four pure virtuals — every implementation must add them. The only known implementation is `JoltPhysicsEngine`; tests using a mock will need to be updated.
- `ColliderShape::Mesh` was previously falling back to `Box`. **Any caller currently using `ColliderShape::Mesh` was silently getting a box** — and will now (correctly) get a mesh, *or* fail body creation if `shapeID == ~0u`. This is a behavioral change but it's a bug fix; the previous behavior was undocumented and incorrect.

## 7. Alternatives considered

### 7.1 Inline children inside `ColliderComponent`

Pack a `std::vector<CompoundChild>` directly into the component. **Rejected:** breaks the fixed-size POD invariant that the ECS storage relies on, kills cache locality for primitive-shape iterations, and forces every entity to pay vector-header bytes.

### 7.2 One ECS entity per child + parent reference

Add a `ParentBodyComponent` that says "I am a child of body X". **Rejected:** doesn't actually reduce body count or query cost — Jolt still sees N bodies. Solves a different problem (hierarchical motion) than this one (collision shape composition).

### 7.3 Reuse `RenderResources::Mesh` for physics meshes

Both rendering and physics consume vertex/index data — could share. **Rejected:** render meshes upload to GPU and may discard CPU-side data; physics meshes need CPU data permanently. Render meshes also carry extra surface attributes (normals, tangents, UVs) the physics shape doesn't want. The two domains have different optimal layouts; the small duplication is worth the decoupling.

### 7.4 Bake per-level mesh files

Store mesh-shape data in a `.physmesh` asset loaded by the engine. **Deferred, not rejected:** orthogonal to this proposal. Once `createMeshShape` exists, an asset format on top is straightforward. Out of scope here.

## 8. Open questions

1. **`MeshShape` quality knobs.** Jolt's `MeshShapeSettings` exposes `mMaxTrianglesPerLeaf` and similar tuning. Default for now; expose later if a game needs it.
2. **Convex hull.** Worth adding `createConvexHullShape(positions, count) → shapeID` in this same change set, since the API shape is identical? Lean yes — same plumbing.
3. **Profiling integration.** Does `PhysicsSystem::registerNewBodies` need a profiling zone covering compound child enumeration? Probably not; compound creation happens once at level load.

## 9. Migration plan for `sample_game`

After this lands:

1. Replace the 128-entity collider loop in `spawnFigureEightFloor` with a single `createCompoundShape` call + one entity (≈30 lines of code → ≈10).
2. Drop `kPhysRadial` and `dRPhys` workarounds.
3. Verify ball-vs-floor friction / restitution behavior matches (these become *body-level* properties; child shapes inherit).
4. Measure: expect `Physics` row in the perf overlay to fall to ~0.

## 10. Estimated effort

- `JoltPhysicsEngine` shape registry + `createMeshShape` + `createCompoundShape` + `addBody` switch: ~150 lines.
- `IPhysicsEngine` virtuals + `BodyDesc` field: ~20 lines.
- `ColliderComponent` field + asserts: ~5 lines.
- Tests (Catch2): one round-trip per shape kind, plus a "destroy before body" and a "destroy after body" lifetime test: ~100 lines.
- Docs: AGENTS.md component reference + a minimal compound-shape example: ~30 lines.

Total: ~300 lines of engine code, half a day of focused work.
