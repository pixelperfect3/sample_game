#include "SampleGame.h"

#include <cstdio>
#include <fstream>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "engine/assets/GltfLoader.h"
#include "engine/audio/IAudioEngine.h"
#include "engine/core/Engine.h"
#include "engine/ecs/Registry.h"
#include "engine/input/InputState.h"
#include "engine/input/Key.h"
#include "engine/physics/PhysicsComponents.h"
#include "engine/rendering/EcsComponents.h"
#include "engine/rendering/Material.h"
#include "engine/rendering/MeshBuilder.h"
#include "engine/rendering/RenderPass.h"
#include "engine/rendering/ViewIds.h"
#include "engine/scene/SceneSerializer.h"
#include "engine/scene/TransformSystem.h"
#include "imgui.h"

using namespace engine::assets;
using namespace engine::core;
using namespace engine::ecs;
using namespace engine::input;
using namespace engine::physics;
using namespace engine::rendering;

namespace
{
std::vector<uint8_t> readFileBytes(const char* path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return {};
    const std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}
void registerCustomComponents(engine::scene::SceneSerializer& ser, uint32_t cubeMeshId)
{
    using namespace engine::ecs;
    using namespace engine::rendering;
    using namespace engine::physics;
    using namespace engine::io;

    // ---- MeshComponent (all level tiles are cubes) -------------------------
    ser.registerComponent(
        "Mesh",
        [](EntityID e, const Registry& reg, const RenderResources&, JsonWriter& w)
        {
            if (!reg.get<MeshComponent>(e)) return;
            w.key("Mesh"); w.startObject();
            w.key("type"); w.writeString("cube");
            w.endObject();
        },
        [cubeMeshId](EntityID e, Registry& reg, RenderResources&,
                     engine::assets::AssetManager&, JsonValue)
        {
            reg.emplace<MeshComponent>(e, MeshComponent{cubeMeshId});
        });

    // ---- MaterialComponent (inline PBR properties) -------------------------
    ser.registerComponent(
        "Material",
        [](EntityID e, const Registry& reg, const RenderResources& res, JsonWriter& w)
        {
            const auto* mc = reg.get<MaterialComponent>(e);
            if (!mc) return;
            const Material* mat = res.getMaterial(mc->material);
            if (!mat) return;
            w.key("Material"); w.startObject();
            w.key("albedo"); w.writeVec4(mat->albedo);
            w.key("roughness"); w.writeFloat(mat->roughness);
            w.key("metallic"); w.writeFloat(mat->metallic);
            w.key("emissiveScale"); w.writeFloat(mat->emissiveScale);
            w.key("transparent"); w.writeUint(mat->transparent);
            w.endObject();
        },
        [](EntityID e, Registry& reg, RenderResources& res,
           engine::assets::AssetManager&, JsonValue val)
        {
            Material mat;
            mat.albedo = val.hasMember("albedo") ? val["albedo"].getVec4()
                                                 : engine::math::Vec4(1.0f);
            mat.roughness = val["roughness"].getFloat(0.5f);
            mat.metallic = val["metallic"].getFloat(0.0f);
            mat.emissiveScale = val["emissiveScale"].getFloat(0.0f);
            mat.transparent = static_cast<uint8_t>(val["transparent"].getUint(0));
            const uint32_t matId = res.addMaterial(mat);
            reg.emplace<MaterialComponent>(e, MaterialComponent{matId});
        });

    // ---- RigidBodyComponent ------------------------------------------------
    ser.registerComponent(
        "RigidBody",
        [](EntityID e, const Registry& reg, const RenderResources&, JsonWriter& w)
        {
            const auto* rb = reg.get<RigidBodyComponent>(e);
            if (!rb) return;
            w.key("RigidBody"); w.startObject();
            w.key("mass"); w.writeFloat(rb->mass);
            w.key("type"); w.writeUint(static_cast<uint32_t>(rb->type));
            w.key("friction"); w.writeFloat(rb->friction);
            w.key("restitution"); w.writeFloat(rb->restitution);
            w.key("linearDamping"); w.writeFloat(rb->linearDamping);
            w.key("angularDamping"); w.writeFloat(rb->angularDamping);
            w.key("layer"); w.writeUint(rb->layer);
            w.endObject();
        },
        [](EntityID e, Registry& reg, RenderResources&,
           engine::assets::AssetManager&, JsonValue val)
        {
            RigidBodyComponent rb;
            rb.mass = val["mass"].getFloat(1.0f);
            rb.type = static_cast<BodyType>(val["type"].getUint(0));
            rb.friction = val["friction"].getFloat(0.5f);
            rb.restitution = val["restitution"].getFloat(0.3f);
            rb.linearDamping = val["linearDamping"].getFloat(0.05f);
            rb.angularDamping = val["angularDamping"].getFloat(0.05f);
            rb.layer = static_cast<uint8_t>(val["layer"].getUint(0));
            reg.emplace<RigidBodyComponent>(e, rb);
        });

    // ---- ColliderComponent -------------------------------------------------
    ser.registerComponent(
        "Collider",
        [](EntityID e, const Registry& reg, const RenderResources&, JsonWriter& w)
        {
            const auto* col = reg.get<ColliderComponent>(e);
            if (!col) return;
            w.key("Collider"); w.startObject();
            w.key("shape"); w.writeUint(static_cast<uint32_t>(col->shape));
            w.key("offset"); w.writeVec3(col->offset);
            w.key("halfExtents"); w.writeVec3(col->halfExtents);
            w.key("radius"); w.writeFloat(col->radius);
            w.key("isSensor"); w.writeUint(col->isSensor);
            w.endObject();
        },
        [](EntityID e, Registry& reg, RenderResources&,
           engine::assets::AssetManager&, JsonValue val)
        {
            ColliderComponent col;
            col.shape = static_cast<ColliderShape>(val["shape"].getUint(0));
            col.offset = val.hasMember("offset") ? val["offset"].getVec3()
                                                 : engine::math::Vec3(0.0f);
            col.halfExtents = val.hasMember("halfExtents") ? val["halfExtents"].getVec3()
                                                           : engine::math::Vec3(0.5f);
            col.radius = val["radius"].getFloat(0.5f);
            col.isSensor = static_cast<uint8_t>(val["isSensor"].getUint(0));
            reg.emplace<ColliderComponent>(e, col);
        });

    // ---- VisibleTag (presence-only) ----------------------------------------
    ser.registerComponent(
        "Visible",
        [](EntityID e, const Registry& reg, const RenderResources&, JsonWriter& w)
        {
            if (!reg.has<VisibleTag>(e)) return;
            w.key("Visible"); w.startObject(); w.endObject();
        },
        [](EntityID e, Registry& reg, RenderResources&,
           engine::assets::AssetManager&, JsonValue)
        {
            reg.emplace<VisibleTag>(e);
        });

    // ---- ShadowVisibleTag --------------------------------------------------
    ser.registerComponent(
        "ShadowVisible",
        [](EntityID e, const Registry& reg, const RenderResources&, JsonWriter& w)
        {
            const auto* sv = reg.get<ShadowVisibleTag>(e);
            if (!sv) return;
            w.key("ShadowVisible"); w.startObject();
            w.key("cascadeMask"); w.writeUint(sv->cascadeMask);
            w.endObject();
        },
        [](EntityID e, Registry& reg, RenderResources&,
           engine::assets::AssetManager&, JsonValue val)
        {
            reg.emplace<ShadowVisibleTag>(e,
                ShadowVisibleTag{static_cast<uint8_t>(val["cascadeMask"].getUint(0xFF))});
        });
}

}  // namespace

SampleGame::SampleGame()
{
    assets_.registerLoader(std::make_unique<GltfLoader>());
}

SampleGame::~SampleGame() = default;

void SampleGame::onInit(Engine& engine, Registry& registry)
{
    engine_ = &engine;
    registry_ = &registry;

    // ---- Kick off glTF loads (async; applied once Ready) ------------------
    sphereHandle_ = assets_.load<GltfAsset>("assets/models/sphere.glb");
    coinHandle_ = assets_.load<GltfAsset>("assets/models/coin.glb");

    // ---- Audio ------------------------------------------------------------
    if (audio_.init())
    {
        const auto bytes = readFileBytes("assets/beep.wav");
        if (!bytes.empty())
            beepClipId_ = audio_.loadClip(bytes.data(), bytes.size());
        else
            std::fprintf(stderr, "SampleGame: failed to read assets/beep.wav\n");
    }

    // ---- IBL --------------------------------------------------------------
    ibl_.generateDefault();

    // ---- UI (MSDF font + renderer) ---------------------------------------
    if (hudFont_.loadFromFile("assets/fonts/JetBrainsMono-msdf.json",
                              "assets/fonts/JetBrainsMono-msdf.png"))
    {
        hudFontLoaded_ = true;
    }
    else
    {
        std::fprintf(stderr, "SampleGame: failed to load MSDF font\n");
    }
    uiRenderer_.init();

    // ---- Shared cube mesh -------------------------------------------------
    MeshData cubeData = makeCubeMeshData();
    Mesh cubeMesh = buildMesh(cubeData);
    cubeMeshId_ = engine.resources().addMesh(std::move(cubeMesh));
    const uint32_t cubeMeshId = cubeMeshId_;

    // ---- Materials --------------------------------------------------------
    Material greyMat;
    greyMat.albedo = {0.55f, 0.55f, 0.58f, 1.0f};
    greyMat.roughness = 0.7f;
    greyMat.metallic = 0.1f;
    greyMatId_ = engine.resources().addMaterial(greyMat);
    const uint32_t greyMatId = greyMatId_;

    Material groundMat;
    groundMat.albedo = {0.22f, 0.22f, 0.25f, 1.0f};
    groundMat.roughness = 0.9f;
    groundMat.metallic = 0.0f;
    groundMatId_ = engine.resources().addMaterial(groundMat);
    const uint32_t groundMatId = groundMatId_;

    Material coinMat;
    coinMat.albedo = {1.0f, 0.85f, 0.2f, 1.0f};
    coinMat.roughness = 0.35f;
    coinMat.metallic = 0.9f;
    coinMatId_ = engine.resources().addMaterial(coinMat);

    Material ballMat;
    ballMat.albedo = {0.85f, 0.15f, 0.15f, 1.0f};
    ballMat.roughness = 0.4f;
    ballMat.metallic = 0.2f;
    ballMatId_ = engine.resources().addMaterial(ballMat);

    // Placeholder mesh IDs (ball & coin) — swapped to glTF IDs once loaded.
    coinMeshId_ = cubeMeshId_;
    ballMeshId_ = cubeMeshId_;

    // ---- Physics ----------------------------------------------------------
    if (!physics_.init())
    {
        std::fprintf(stderr, "SampleGame: failed to initialize Jolt physics\n");
        return;
    }

    // ---- Generate scene files for both levels ------------------------------
    {
        // Plank level
        Registry tempReg;
        spawnPlankLevel(tempReg);

        engine::scene::SceneSerializer ser;
        ser.registerEngineComponents();
        registerCustomComponents(ser, cubeMeshId);
        ser.saveScene(tempReg, engine.resources(), "levels/plank.json");
        std::fprintf(stderr, "SampleGame: saved levels/plank.json\n");
    }
    {
        // Figure-8 level
        Registry tempReg;
        spawnFigureEightFloor(tempReg, cubeMeshId, greyMatId);

        engine::scene::SceneSerializer ser;
        ser.registerEngineComponents();
        registerCustomComponents(ser, cubeMeshId);
        ser.saveScene(tempReg, engine.resources(), "levels/figure8.json");
        std::fprintf(stderr, "SampleGame: saved levels/figure8.json\n");
    }

    // Don't load a level yet — title screen shows first.
    // loadLevel() is called when the player clicks "Start Game".
}

void SampleGame::applyLoadedAssets(Engine& engine, Registry& registry)
{
    if (assets_.state(sphereHandle_) != AssetState::Ready ||
        assets_.state(coinHandle_) != AssetState::Ready)
    {
        return;
    }

    const GltfAsset* sphere = assets_.get<GltfAsset>(sphereHandle_);
    const GltfAsset* coin = assets_.get<GltfAsset>(coinHandle_);
    if (!sphere || sphere->meshes.empty() || !coin || coin->meshes.empty())
    {
        std::fprintf(stderr, "SampleGame: loaded asset has no mesh\n");
        assetsApplied_ = true;  // don't keep retrying
        return;
    }

    // Register the GLB's mesh+material into RenderResources.
    // The GltfAsset retains ownership of the bgfx handles (must outlive entities).
    ballMeshId_ = engine.resources().addMesh(Mesh(sphere->meshes[0]));
    if (!sphere->materials.empty())
    {
        Material sphereMat = sphere->materials[0];
        sphereMat.transparent = 1;
        sphereMat.albedo.w = 0.75f;  // ~75% opaque — coin still visible through it
        ballMatId_ = engine.resources().addMaterial(sphereMat);
    }

    coinMeshId_ = engine.resources().addMesh(Mesh(coin->meshes[0]));
    if (!coin->materials.empty())
        coinMatId_ = engine.resources().addMaterial(coin->materials[0]);

    // Swap the live ball entity over to the sphere mesh/material.
    if (auto* mc = registry.get<MeshComponent>(ballEntity_))
        mc->mesh = ballMeshId_;
    if (auto* mm = registry.get<MaterialComponent>(ballEntity_))
        mm->material = ballMatId_;

    // Swap each live coin's mesh/material.
    for (int i = 0; i < coinCount_; ++i)
    {
        if (coinEntities_[i] == 0) continue;
        if (auto* mc = registry.get<MeshComponent>(coinEntities_[i]))
            mc->mesh = coinMeshId_;
        if (auto* mm = registry.get<MaterialComponent>(coinEntities_[i]))
            mm->material = coinMatId_;
    }

    assetsApplied_ = true;
}

void SampleGame::spawnFigureEightFloor(Registry& registry, uint32_t meshId, uint32_t matId)
{
    // Tessellate each ring into angular×radial wedges. Each wedge is a small
    // box oriented tangent to the ring, so edges follow the ring curvature
    // (much smoother than an axis-aligned grid).
    constexpr int kAngular = 64;          // angular segments per ring
    constexpr int kRadial = 3;            // radial layers across the ring width
    constexpr float kROuter = 4.0f;
    constexpr float kRInner = 2.5f;
    constexpr float kCx = 3.5f;
    constexpr float kThick = 1.0f;
    constexpr float kTau = 6.2831853f;
    const float dR = (kROuter - kRInner) / static_cast<float>(kRadial);
    const float dTheta = kTau / static_cast<float>(kAngular);
    const float halfThick = kThick * 0.5f;
    const glm::vec3 centers[2] = {{-kCx, 0.0f, 0.0f}, {kCx, 0.0f, 0.0f}};

    for (int circle = 0; circle < 2; ++circle)
    {
        const glm::vec3 center = centers[circle];
        for (int a = 0; a < kAngular; ++a)
        {
            const float theta = dTheta * (static_cast<float>(a) + 0.5f);
            const float cosT = std::cos(theta);
            const float sinT = std::sin(theta);
            for (int r = 0; r < kRadial; ++r)
            {
                const float rMid = kRInner + dR * (static_cast<float>(r) + 0.5f);
                const float arcLen = dTheta * rMid;

                EntityID tile = registry.createEntity();
                TransformComponent tc{};
                tc.position = {center.x + rMid * cosT, -halfThick, center.z + rMid * sinT};
                tc.rotation = glm::angleAxis(-theta, glm::vec3(0.0f, 1.0f, 0.0f));
                // 2% overlap in both dims prevents visible seams and
                // guarantees no crack the ball could fall through.
                tc.scale = {dR * 1.02f, kThick, arcLen * 1.02f};
                tc.flags = 1;
                registry.emplace<TransformComponent>(tile, tc);
                registry.emplace<WorldTransformComponent>(tile);
                registry.emplace<MeshComponent>(tile, MeshComponent{meshId});
                registry.emplace<MaterialComponent>(tile, MaterialComponent{matId});
                registry.emplace<VisibleTag>(tile);
                registry.emplace<ShadowVisibleTag>(tile, ShadowVisibleTag{0xFF});

                RigidBodyComponent rb;
                rb.mass = 0.0f;
                rb.type = BodyType::Kinematic;
                rb.friction = 0.8f;
                rb.restitution = 0.1f;
                registry.emplace<RigidBodyComponent>(tile, rb);

                ColliderComponent col;
                col.shape = ColliderShape::Box;
                col.halfExtents = {dR * 0.51f, halfThick, arcLen * 0.51f};
                registry.emplace<ColliderComponent>(tile, col);
            }
        }
    }
}

void SampleGame::spawnPlankLevel(Registry& registry)
{
    // Plank only — no large ground plane
    {
        EntityID plank = registry.createEntity();
        TransformComponent tc{};
        tc.position = {0.0f, 2.0f, 0.0f};
        tc.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        tc.scale = {6.0f, 0.15f, 1.0f};
        tc.flags = 1;
        registry.emplace<TransformComponent>(plank, tc);
        registry.emplace<WorldTransformComponent>(plank);
        registry.emplace<MeshComponent>(plank, MeshComponent{cubeMeshId_});
        registry.emplace<MaterialComponent>(plank, MaterialComponent{greyMatId_});
        registry.emplace<VisibleTag>(plank);
        registry.emplace<ShadowVisibleTag>(plank, ShadowVisibleTag{0xFF});

        RigidBodyComponent rb;
        rb.mass = 0.0f;
        rb.type = BodyType::Kinematic;
        rb.friction = 0.8f;
        rb.restitution = 0.1f;
        registry.emplace<RigidBodyComponent>(plank, rb);

        ColliderComponent col;
        col.shape = ColliderShape::Box;
        col.halfExtents = {3.0f, 0.075f, 0.5f};
        registry.emplace<ColliderComponent>(plank, col);
    }
}

void SampleGame::spawnBall(Registry& registry)
{
    ballEntity_ = registry.createEntity();
    const float r = 0.55f;
    TransformComponent tc{};
    tc.position = {ballStartPos_.x, ballStartPos_.y, ballStartPos_.z};
    tc.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    tc.scale = {r * 2.0f, r * 2.0f, r * 2.0f};
    tc.flags = 1;
    registry.emplace<TransformComponent>(ballEntity_, tc);
    registry.emplace<WorldTransformComponent>(ballEntity_);
    registry.emplace<MeshComponent>(ballEntity_, MeshComponent{ballMeshId_});
    registry.emplace<MaterialComponent>(ballEntity_, MaterialComponent{ballMatId_});
    registry.emplace<VisibleTag>(ballEntity_);
    registry.emplace<ShadowVisibleTag>(ballEntity_, ShadowVisibleTag{0xFF});

    RigidBodyComponent rb;
    rb.mass = 15.0f;
    rb.type = BodyType::Dynamic;
    rb.friction = 0.4f;
    rb.restitution = 0.3f;
    rb.linearDamping = 0.05f;
    rb.angularDamping = 0.05f;
    registry.emplace<RigidBodyComponent>(ballEntity_, rb);

    ColliderComponent col;
    col.shape = ColliderShape::Sphere;
    col.radius = r;
    registry.emplace<ColliderComponent>(ballEntity_, col);
}

void SampleGame::clearLevel(Registry& registry)
{
    // Destroy every entity in the registry — nothing persists across levels.
    std::vector<EntityID> all;
    registry.forEachEntity([&](EntityID e) { all.push_back(e); });
    for (auto e : all)
        registry.destroyEntity(e);

    // Reset handles.
    coinEntities_.fill(0);
    coinCollectedFlags_.fill(false);
    coinsRemaining_ = 0;
    ballEntity_ = 0;
    groundEntity_ = 0;
    levelEntities_.clear();
}

void SampleGame::loadLevel(Engine& engine, Registry& registry, int level)
{
    clearLevel(registry);
    currentLevel_ = level;

    if (level == 0)
    {
        // Plank level
        ballStartPos_ = {-2.4f, 2.7f, 0.0f};
        coinCount_ = 1;

        engine::scene::SceneSerializer ser;
        ser.registerEngineComponents();
        registerCustomComponents(ser, cubeMeshId_);
        ser.loadScene("levels/plank.json", registry,
                      engine.resources(), assets_);
    }
    else if (level == 1)
    {
        // Figure-8 level
        ballStartPos_ = {-7.0f, 0.55f, 0.0f};
        coinCount_ = 3;

        engine::scene::SceneSerializer ser;
        ser.registerEngineComponents();
        registerCustomComponents(ser, cubeMeshId_);
        ser.loadScene("levels/figure8.json", registry,
                      engine.resources(), assets_);

        // Invisible safety floor at y=-20
        groundEntity_ = registry.createEntity();
        TransformComponent tc{};
        tc.position = {0.0f, -20.0f, 0.0f};
        tc.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        tc.scale = {100.0f, 1.0f, 100.0f};
        tc.flags = 1;
        registry.emplace<TransformComponent>(groundEntity_, tc);
        registry.emplace<WorldTransformComponent>(groundEntity_);

        RigidBodyComponent rb;
        rb.mass = 0.0f;
        rb.type = BodyType::Kinematic;
        rb.friction = 0.5f;
        rb.restitution = 0.1f;
        registry.emplace<RigidBodyComponent>(groundEntity_, rb);

        ColliderComponent col;
        col.shape = ColliderShape::Box;
        col.halfExtents = {50.0f, 0.5f, 50.0f};
        registry.emplace<ColliderComponent>(groundEntity_, col);
    }

    spawnBall(registry);
    spawnAllCoins(registry);

    engine::scene::TransformSystem transformSys;
    transformSys.update(registry);
}

void SampleGame::spawnAllCoins(Registry& registry)
{
    for (int i = 0; i < coinCount_; ++i)
        spawnCoin(registry, i);
    coinsRemaining_ = coinCount_;
    coinCollectedFlags_.fill(false);

    // Snap smoothed camera target to the nearest coin — no swing on reset.
    const glm::vec3 ballStart = ballStartPos_;
    int nearest = 0;
    float bestSq = 1e30f;
    for (int i = 0; i < coinCount_; ++i)
    {
        const glm::vec3 d = coinPositions_[i] - ballStart;
        const float sq = glm::dot(d, d);
        if (sq < bestSq) { bestSq = sq; nearest = i; }
    }
    smoothedTargetPos_ = coinPositions_[nearest];
    glm::vec2 fwd{smoothedTargetPos_.x - ballStart.x, smoothedTargetPos_.z - ballStart.z};
    const float l = glm::length(fwd);
    smoothedFwd_ = (l > 1e-4f) ? (fwd / l) : glm::vec2(0.0f, -1.0f);
}

void SampleGame::spawnCoin(Registry& registry, int index)
{
    const float r = 0.2f;

    if (currentLevel_ == 0)
    {
        // Plank level: single coin at fixed position
        coinPositions_[index] = {2.6f, 2.35f, 0.0f};
    }
    else
    {
        // Figure-8 level: random ring placement
        constexpr float kROuter = 4.0f;
        constexpr float kRInner = 2.5f;
        constexpr float kCx = 3.5f;
        std::uniform_real_distribution<float> angleDist(0.0f, 6.2831853f);
        std::uniform_real_distribution<float> radiusDist(
            kRInner + 0.4f, kROuter - 0.4f);
        std::uniform_int_distribution<int> ringDist(0, 1);
        const float theta = angleDist(rng_);
        const float radius = radiusDist(rng_);
        const int ring = (index == 0) ? 1 : ringDist(rng_);
        const float cx = (ring == 0) ? -kCx : kCx;
        coinPositions_[index] = {cx + radius * std::cos(theta), 0.55f,
                                 radius * std::sin(theta)};
    }

    const EntityID coin = registry.createEntity();
    coinEntities_[index] = coin;

    TransformComponent tc{};
    tc.position = {coinPositions_[index].x, coinPositions_[index].y,
                   coinPositions_[index].z};
    // Stand the coin upright, then spin it 90° around Y.
    tc.rotation =
        glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    tc.scale = {r * 2.0f, r * 2.0f, r * 2.0f};
    tc.flags = 1;
    registry.emplace<TransformComponent>(coin, tc);
    registry.emplace<WorldTransformComponent>(coin);
    registry.emplace<MeshComponent>(coin, MeshComponent{coinMeshId_});
    registry.emplace<MaterialComponent>(coin, MaterialComponent{coinMatId_});
    registry.emplace<VisibleTag>(coin);
    registry.emplace<ShadowVisibleTag>(coin, ShadowVisibleTag{0xFF});

    RigidBodyComponent rb;
    rb.mass = 0.0f;
    rb.type = BodyType::Kinematic;
    rb.friction = 0.5f;
    rb.restitution = 0.3f;
    registry.emplace<RigidBodyComponent>(coin, rb);

    ColliderComponent col;
    col.shape = ColliderShape::Sphere;
    col.radius = r;
    col.isSensor = 1;  // overlap only — ball passes through, no impulse
    registry.emplace<ColliderComponent>(coin, col);
}

void SampleGame::onFixedUpdate(Engine& engine, Registry& registry, float fixedDt)
{
    if (showTitleScreen_) return;

    // Apply directional force to the ball while movement keys are held.
    // Force is persistent per fixed step, so holding a direction accelerates.
    const auto& input = engine.inputState();
    glm::vec3 force{0.0f};
    constexpr float kForceMag = 45.0f;  // N; mass=15 → 3.0 m/s² of acceleration
    // Movement is camera-relative. smoothedFwd_ is the XZ unit vector from
    // ball toward the current look-target. In a right-handed world with
    // Y up, the camera's "right" = cross(forward, up), which in XZ is
    // (-fwd.z, fwd.x). Using (fwd.x, fwd.y) as XZ:
    const glm::vec2 fwd = smoothedFwd_;
    const glm::vec2 right{-fwd.y, fwd.x};
    float axisF = 0.0f, axisR = 0.0f;
    if (input.isKeyHeld(Key::Up) || input.isKeyHeld(Key::W))    axisF += 1.0f;
    if (input.isKeyHeld(Key::Down) || input.isKeyHeld(Key::S))  axisF -= 1.0f;
    if (input.isKeyHeld(Key::Right) || input.isKeyHeld(Key::D)) axisR += 1.0f;
    if (input.isKeyHeld(Key::Left) || input.isKeyHeld(Key::A))  axisR -= 1.0f;
    force.x = (axisF * fwd.x + axisR * right.x) * kForceMag;
    force.z = (axisF * fwd.y + axisR * right.y) * kForceMag;

    if (force.x != 0.0f || force.z != 0.0f)
    {
        if (auto* rb = registry.get<RigidBodyComponent>(ballEntity_); rb && rb->bodyID != ~0u)
            physics_.applyForce(rb->bodyID, {force.x, 0.0f, force.z});
    }

    physicsSys_.update(registry, physics_, fixedDt);

    // If the ball falls off the figure-8:
    //   - Mid-game: reset the level.
    //   - All coins collected: let the ball fall freely, freeze physics at y=-20.
    if (auto* tc = registry.get<TransformComponent>(ballEntity_))
    {
        if (tc->position.y < -10.0f && coinsRemaining_ > 0)
        {
            resetLevel(registry);
            return;
        }
        if (tc->position.y < -20.0f && coinsRemaining_ == 0)
        {
            if (auto* rb = registry.get<RigidBodyComponent>(ballEntity_);
                rb && rb->bodyID != ~0u)
            {
                physics_.setLinearVelocity(rb->bodyID, {0.0f, 0.0f, 0.0f});
                physics_.setAngularVelocity(rb->bodyID, {0.0f, 0.0f, 0.0f});
            }
            return;
        }
    }

    // Detect ball-vs-coin contacts — play the beep and remove any hit coin.
    for (const auto& evt : physics_.getContactBeginEvents())
    {
        EntityID other = 0;
        if (evt.entityA == ballEntity_)      other = evt.entityB;
        else if (evt.entityB == ballEntity_) other = evt.entityA;
        else continue;

        for (int i = 0; i < coinCount_; ++i)
        {
            if (coinCollectedFlags_[i] || coinEntities_[i] != other)
                continue;

            if (beepClipId_ != 0)
                audio_.play(beepClipId_, engine::audio::SoundCategory::SFX, 1.0f, false);

            registry.destroyEntity(coinEntities_[i]);
            coinEntities_[i] = 0;
            coinCollectedFlags_[i] = true;
            --coinsRemaining_;
            break;
        }
    }
}

void SampleGame::onUpdate(Engine& engine, Registry& registry, float dt)
{
    if (showTitleScreen_) return;

    // Drain async asset uploads; apply GLB meshes once Ready.
    assets_.processUploads();
    if (!assetsApplied_)
        applyLoadedAssets(engine, registry);

    // Pick the nearest uncollected coin and smoothly track it.
    glm::vec3 ballPos = ballStartPos_;
    if (auto* tc = registry.get<TransformComponent>(ballEntity_))
        ballPos = glm::vec3(tc->position.x, tc->position.y, tc->position.z);

    if (coinsRemaining_ > 0)
    {
        int nearest = -1;
        float bestSq = 1e30f;
        for (int i = 0; i < coinCount_; ++i)
        {
            if (coinCollectedFlags_[i]) continue;
            const glm::vec3 d = coinPositions_[i] - ballPos;
            const float sq = glm::dot(d, d);
            if (sq < bestSq) { bestSq = sq; nearest = i; }
        }
        if (nearest >= 0)
        {
            const glm::vec3 target = coinPositions_[nearest];
            const float kTarget = 1.0f - std::exp(-3.0f * dt);
            smoothedTargetPos_ = glm::mix(smoothedTargetPos_, target, kTarget);
        }
    }
    // Recompute smoothedFwd_ from the ball→target XZ direction.
    {
        glm::vec2 want{smoothedTargetPos_.x - ballPos.x,
                       smoothedTargetPos_.z - ballPos.z};
        const float l = glm::length(want);
        if (l > 1e-3f)
        {
            want /= l;
            const float kDir = 1.0f - std::exp(-4.0f * dt);
            smoothedFwd_ = glm::normalize(glm::mix(smoothedFwd_, want, kDir));
        }
    }

    // Spin all remaining coins around Y at 180°/s.
    coinSpinTime_ += dt;
    const float spinDeg = coinSpinTime_ * 180.0f;
    const glm::quat spinRot =
        glm::angleAxis(glm::radians(spinDeg), glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    for (int i = 0; i < coinCount_; ++i)
    {
        if (coinCollectedFlags_[i]) continue;
        if (auto* tc = registry.get<TransformComponent>(coinEntities_[i]))
        {
            tc->rotation = spinRot;
            tc->flags |= 1;
        }
    }

    const auto& input = engine.inputState();

    // R key resets everything.
    if (input.isKeyPressed(Key::R))
        resetLevel(registry);
}

void SampleGame::resetLevel(Registry& registry)
{
    if (auto* rb = registry.get<RigidBodyComponent>(ballEntity_); rb && rb->bodyID != ~0u)
    {
        physics_.setBodyPosition(rb->bodyID,
            {ballStartPos_.x, ballStartPos_.y, ballStartPos_.z});
        physics_.setBodyRotation(rb->bodyID, {1.0f, 0.0f, 0.0f, 0.0f});
        physics_.setLinearVelocity(rb->bodyID, {0.0f, 0.0f, 0.0f});
        physics_.setAngularVelocity(rb->bodyID, {0.0f, 0.0f, 0.0f});
    }
    for (int i = 0; i < coinCount_; ++i)
    {
        if (coinEntities_[i] != 0)
            registry.destroyEntity(coinEntities_[i]);
        coinEntities_[i] = 0;
    }
    spawnAllCoins(registry);
}

void SampleGame::onRender(Engine& engine)
{
    engine.renderer().beginFrameDirect();

    const auto W = engine.fbWidth();
    const auto H = engine.fbHeight();
    const float fbW = static_cast<float>(W);
    const float fbH = static_cast<float>(H);

    // ---- Title screen -----------------------------------------------------
    if (showTitleScreen_)
    {
        // Just clear the screen and show the title UI.
        RenderPass(kViewOpaque)
            .rect(0, 0, W, H)
            .clearColorAndDepth(0x1A1A2EFF);

        const float centerX = fbW * 0.5f;
        const float centerY = fbH * 0.35f;

        ImGui::SetNextWindowPos(ImVec2(centerX, centerY), ImGuiCond_Always,
                                ImVec2(0.5f, 0.5f));
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoBackground |
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_AlwaysAutoResize;
        if (ImGui::Begin("##title", nullptr, flags))
        {
            ImGui::SetWindowFontScale(12.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.95f, 0.3f, 1.0f), "Sample Game");

            ImGui::SetWindowFontScale(6.0f);
            ImGui::Dummy(ImVec2(0, 40));

            // Centre the button by calculating its width.
            const float buttonW = 400.0f;
            const float buttonH = 80.0f;
            ImGui::SetCursorPosX(
                (ImGui::GetWindowWidth() - buttonW) * 0.5f);
            if (ImGui::Button("Start Game", ImVec2(buttonW, buttonH)))
            {
                showTitleScreen_ = false;
                if (engine_ && registry_)
                    loadLevel(*engine_, *registry_, 0);
            }

            ImGui::Dummy(ImVec2(0, 50));

            ImGui::SetWindowFontScale(3.0f);
            ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f),
                "Roll the ball to collect all the coins.");
            ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f),
                "Don't fall off the edge!");

            ImGui::Dummy(ImVec2(0, 30));

            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.8f, 1.0f), "Controls");
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f),
                "W / Up      Move forward");
            ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f),
                "S / Down    Move backward");
            ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f),
                "A / Left    Move left");
            ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f),
                "D / Right   Move right");
            ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f),
                "R           Reset level");
        }
        ImGui::End();
        return;  // skip 3D rendering
    }

    // Chase camera: behind the ball along -smoothedFwd_, looking toward
    // the smoothed target (nearest uncollected coin).
    glm::vec3 ballPos = ballStartPos_;
    if (registry_)
    {
        if (auto* tc = registry_->get<TransformComponent>(ballEntity_))
            ballPos = glm::vec3(tc->position.x, tc->position.y, tc->position.z);
    }
    const glm::vec3 fwd3{smoothedFwd_.x, 0.0f, smoothedFwd_.y};
    const glm::vec3 camPos = ballPos - fwd3 * 6.0f + glm::vec3(0.0f, 3.5f, 0.0f);
    const glm::mat4 viewMat = glm::lookAt(camPos, smoothedTargetPos_, glm::vec3(0, 1, 0));
    const glm::mat4 projMat = glm::perspective(glm::radians(45.f), fbW / fbH, 0.05f, 100.f);

    const glm::vec3 kLightDir = glm::normalize(glm::vec3(1.0f, 2.0f, 1.0f));
    constexpr float kLightIntens = 6.0f;
    const float lightData[8] = {
        kLightDir.x,         kLightDir.y,          kLightDir.z,          0.f,
        1.0f * kLightIntens, 0.95f * kLightIntens, 0.85f * kLightIntens, 0.f};

    const glm::vec3 kLightPos = kLightDir * 20.f;
    const glm::mat4 lightView = glm::lookAt(kLightPos, glm::vec3(0.f), glm::vec3(0, 1, 0));
    const glm::mat4 lightProj = glm::ortho(-12.f, 12.f, -12.f, 12.f, 0.1f, 50.f);

    engine.shadow().beginCascade(0, lightView, lightProj);

    if (!registry_)
        return;

    drawCallSys_.submitShadowDrawCalls(*registry_, engine.resources(), engine.shadowProgram(), 0);

    RenderPass(kViewOpaque)
        .rect(0, 0, W, H)
        .clearColorAndDepth(0x1A1A2EFF)
        .transform(viewMat, projMat);

    // Transparent pass shares the view/proj; no clear (blends onto opaque).
    RenderPass(kViewTransparent)
        .rect(0, 0, W, H)
        .transform(viewMat, projMat);

    const glm::mat4 shadowMat = engine.shadow().shadowMatrix(0);
    PbrFrameParams frame{
        lightData, glm::value_ptr(shadowMat), engine.shadow().atlasTexture(), W, H, 0.05f, 100.f};
    frame.camPos[0] = camPos.x;
    frame.camPos[1] = camPos.y;
    frame.camPos[2] = camPos.z;

    if (ibl_.isValid())
    {
        frame.iblEnabled = true;
        frame.maxMipLevels = 7.0f;
        frame.irradiance = ibl_.irradiance();
        frame.prefiltered = ibl_.prefiltered();
        frame.brdfLut = ibl_.brdfLut();
    }

    drawCallSys_.update(*registry_, engine.resources(), engine.pbrProgram(), engine.uniforms(),
                        frame);

    // ---- HUD --------------------------------------------------------------
    const int collected = coinCount_ - coinsRemaining_;
    const bool levelComplete = (coinsRemaining_ == 0);
    const bool hasNextLevel = (currentLevel_ < kLevelCount - 1);

    // Render HUD text via MSDF UiDrawList when the font is loaded.
    if (hudFontLoaded_)
    {
        hudDrawList_.clear();

        const engine::math::Vec4 white{1.0f, 1.0f, 1.0f, 1.0f};
        const engine::math::Vec4 yellow{1.0f, 0.95f, 0.3f, 1.0f};
        const engine::math::Vec2 hudPos{80.0f, 60.0f};
        const float hudSize = 72.0f;

        if (levelComplete)
        {
            if (hasNextLevel)
            {
                hudDrawList_.drawText(hudPos, "LEVEL COMPLETE!", yellow, &hudFont_, hudSize);
            }
            else
            {
                hudDrawList_.drawText(hudPos, "YOU WIN!", yellow, &hudFont_, hudSize);
            }
        }
        else
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Coins Collected: %d/%d", collected, coinCount_);
            hudDrawList_.drawText(hudPos, buf, white, &hudFont_, hudSize);
        }

        const bgfx::ViewId hudView = engine::rendering::kViewGameUi;
        bgfx::setViewName(hudView, "HUD");
        bgfx::setViewRect(hudView, 0, 0, W, H);
        bgfx::setViewClear(hudView, BGFX_CLEAR_NONE);
        bgfx::touch(hudView);
        uiRenderer_.render(hudDrawList_, hudView, W, H);
    }
    else
    {
        // Fallback: render the HUD text via ImGui when the MSDF font failed.
        ImGui::SetNextWindowPos(ImVec2(80.0f, 60.0f), ImGuiCond_Always);
        ImGuiWindowFlags hudFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoBackground |
                                    ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##hud_fallback", nullptr, hudFlags))
        {
            ImGui::SetWindowFontScale(10.0f);
            if (levelComplete)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.95f, 0.3f, 1.0f),
                                   hasNextLevel ? "LEVEL COMPLETE!" : "YOU WIN!");
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
                                   "Coins Collected: %d/%d",
                                   collected, coinCount_);
            }
        }
        ImGui::End();
    }

    // Next-level button is still ImGui for now (migrated in step 3).
    if (levelComplete && hasNextLevel)
    {
        ImGui::SetNextWindowPos(ImVec2(80.0f, 160.0f), ImGuiCond_Always);
        ImGuiWindowFlags btnFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoBackground |
                                    ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_AlwaysAutoResize;
        if (ImGui::Begin("##hud_btn", nullptr, btnFlags))
        {
            ImGui::SetWindowFontScale(5.0f);
            if (ImGui::Button("Next Level"))
            {
                if (engine_ && registry_)
                    loadLevel(*engine_, *registry_, currentLevel_ + 1);
            }
        }
        ImGui::End();
    }
}

void SampleGame::onShutdown(Engine& /*engine*/, Registry& /*registry*/)
{
    uiRenderer_.shutdown();
    if (hudFontLoaded_)
    {
        hudFont_.shutdown();
        hudFontLoaded_ = false;
    }
    ibl_.shutdown();
    physics_.shutdown();
    audio_.shutdown();
}
