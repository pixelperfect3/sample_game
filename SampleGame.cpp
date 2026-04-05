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
#include "engine/scene/TransformSystem.h"

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
}  // namespace

SampleGame::SampleGame()
{
    assets_.registerLoader(std::make_unique<GltfLoader>());
}

SampleGame::~SampleGame() = default;

void SampleGame::onInit(Engine& engine, Registry& registry)
{
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
    const uint32_t greyMatId = engine.resources().addMaterial(greyMat);

    Material groundMat;
    groundMat.albedo = {0.22f, 0.22f, 0.25f, 1.0f};
    groundMat.roughness = 0.9f;
    groundMat.metallic = 0.0f;
    const uint32_t groundMatId = engine.resources().addMaterial(groundMat);

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

    // ---- Safety floor (invisible, far below) ------------------------------
    // Catches the ball if it falls through the hole in a ring.
    groundEntity_ = registry.createEntity();
    {
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

    // ---- Figure-8 ring floor ----------------------------------------------
    spawnFigureEightFloor(registry, cubeMeshId, greyMatId);

    // ---- Coins (3 random spots around the figure-8) -----------------------
    spawnAllCoins(registry);

    // ---- Large ball (at -X end) -------------------------------------------
    ballEntity_ = registry.createEntity();
    {
        const float r = 0.55f;
        TransformComponent tc{};
        tc.position = {-7.0f, 0.55f, 0.0f};  // leftmost edge of the left ring
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

    // Populate WorldTransformComponent for every entity before the first
    // physics step — PhysicsSystem::syncKinematicBodies reads world matrices
    // on every step, and GameRunner otherwise only runs TransformSystem
    // after onUpdate (too late for frame 1, causing kinematic tiles to drift
    // toward the origin).
    engine::scene::TransformSystem transformSys;
    transformSys.update(registry);
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
        ballMatId_ = engine.resources().addMaterial(sphere->materials[0]);

    coinMeshId_ = engine.resources().addMesh(Mesh(coin->meshes[0]));
    if (!coin->materials.empty())
        coinMatId_ = engine.resources().addMaterial(coin->materials[0]);

    // Swap the live ball entity over to the sphere mesh/material.
    if (auto* mc = registry.get<MeshComponent>(ballEntity_))
        mc->mesh = ballMeshId_;
    if (auto* mm = registry.get<MaterialComponent>(ballEntity_))
        mm->material = ballMatId_;

    // Swap each live coin's mesh/material.
    for (int i = 0; i < kCoinCount; ++i)
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
    // Two rings (annuli) forming a "figure 8" — hollow centres = gaps.
    constexpr float kROuter = 4.0f;
    constexpr float kRInner = 2.5f;
    constexpr float kCx = 3.5f;  // circles overlap modestly at x=0 to share the pinch
    constexpr float kCell = 0.4f;
    constexpr float kThick = 1.0f;
    const float halfCell = kCell * 0.5f;
    const float halfThick = kThick * 0.5f;
    const float outer2 = kROuter * kROuter;
    const float inner2 = kRInner * kRInner;

    const float bx = 2.0f * kROuter + kCx;
    const float bz = kROuter;
    for (float x = -bx; x <= bx + 0.001f; x += kCell)
    {
        for (float z = -bz; z <= bz + 0.001f; z += kCell)
        {
            const float dlx = x + kCx, dlz = z;
            const float drx = x - kCx, drz = z;
            const float dL2 = dlx * dlx + dlz * dlz;
            const float dR2 = drx * drx + drz * drz;
            const bool inLeftRing = (dL2 >= inner2 && dL2 <= outer2);
            const bool inRightRing = (dR2 >= inner2 && dR2 <= outer2);
            if (!inLeftRing && !inRightRing) continue;

            EntityID tile = registry.createEntity();
            TransformComponent tc{};
            tc.position = {x, -halfThick, z};  // top surface at y=0
            tc.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            tc.scale = {kCell, kThick, kCell};
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
            col.halfExtents = {halfCell, halfThick, halfCell};
            registry.emplace<ColliderComponent>(tile, col);
        }
    }
}

void SampleGame::spawnAllCoins(Registry& registry)
{
    for (int i = 0; i < kCoinCount; ++i)
        spawnCoin(registry, i);
    coinsRemaining_ = kCoinCount;
    coinCollectedFlags_.fill(false);

    // Snap smoothed camera target to the nearest coin — no swing on reset.
    const glm::vec3 ballStart{-7.0f, 0.55f, 0.0f};
    int nearest = 0;
    float bestSq = 1e30f;
    for (int i = 0; i < kCoinCount; ++i)
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

    // Random point on either ring (50/50) — spread coins around the figure.
    constexpr float kROuter = 4.0f;
    constexpr float kRInner = 2.5f;
    constexpr float kCx = 3.5f;
    std::uniform_real_distribution<float> angleDist(0.0f, 6.2831853f);
    std::uniform_real_distribution<float> radiusDist(kRInner + 0.4f, kROuter - 0.4f);
    std::uniform_int_distribution<int> ringDist(0, 1);
    const float theta = angleDist(rng_);
    const float radius = radiusDist(rng_);
    // Coin 0 always spawns on the right ring (so the ball has at least one
    // target ahead of its starting edge); the rest pick randomly.
    const int ring = (index == 0) ? 1 : ringDist(rng_);
    const float cx = (ring == 0) ? -kCx : kCx;
    coinPositions_[index] = {cx + radius * std::cos(theta), 0.55f,
                             radius * std::sin(theta)};

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

    // Detect ball-vs-coin contacts — play the beep and remove any hit coin.
    for (const auto& evt : physics_.getContactBeginEvents())
    {
        EntityID other = 0;
        if (evt.entityA == ballEntity_)      other = evt.entityB;
        else if (evt.entityB == ballEntity_) other = evt.entityA;
        else continue;

        for (int i = 0; i < kCoinCount; ++i)
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
    // Drain async asset uploads; apply GLB meshes once Ready.
    assets_.processUploads();
    if (!assetsApplied_)
        applyLoadedAssets(engine, registry);

    // Pick the nearest uncollected coin and smoothly track it.
    glm::vec3 ballPos{-7.0f, 0.55f, 0.0f};
    if (auto* tc = registry.get<TransformComponent>(ballEntity_))
        ballPos = glm::vec3(tc->position.x, tc->position.y, tc->position.z);

    if (coinsRemaining_ > 0)
    {
        int nearest = -1;
        float bestSq = 1e30f;
        for (int i = 0; i < kCoinCount; ++i)
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
    for (int i = 0; i < kCoinCount; ++i)
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
    {
        auto resetBody = [&](EntityID id, const glm::vec3& pos)
        {
            auto* rb = registry.get<RigidBodyComponent>(id);
            if (rb && rb->bodyID != ~0u)
            {
                physics_.setBodyPosition(rb->bodyID, {pos.x, pos.y, pos.z});
                physics_.setBodyRotation(rb->bodyID, {1.0f, 0.0f, 0.0f, 0.0f});
                physics_.setLinearVelocity(rb->bodyID, {0.0f, 0.0f, 0.0f});
                physics_.setAngularVelocity(rb->bodyID, {0.0f, 0.0f, 0.0f});
            }
        };
        resetBody(ballEntity_, {-7.0f, 0.55f, 0.0f});

        // Re-roll all coins on reset.
        for (int i = 0; i < kCoinCount; ++i)
        {
            if (coinEntities_[i] != 0)
                registry.destroyEntity(coinEntities_[i]);
            coinEntities_[i] = 0;
        }
        spawnAllCoins(registry);
    }
}

void SampleGame::onRender(Engine& engine)
{
    engine.renderer().beginFrameDirect();

    const auto W = engine.fbWidth();
    const auto H = engine.fbHeight();
    const float fbW = static_cast<float>(W);
    const float fbH = static_cast<float>(H);

    // Chase camera: behind the ball along -smoothedFwd_, looking toward
    // the smoothed target (nearest uncollected coin).
    glm::vec3 ballPos{-7.0f, 0.55f, 0.0f};
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
}

void SampleGame::onShutdown(Engine& /*engine*/, Registry& /*registry*/)
{
    ibl_.shutdown();
    physics_.shutdown();
    audio_.shutdown();
}
