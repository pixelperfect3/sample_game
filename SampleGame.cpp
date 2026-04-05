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

    // ---- Ground (static) --------------------------------------------------
    groundEntity_ = registry.createEntity();
    {
        TransformComponent tc{};
        tc.position = {0.0f, -3.0f, 0.0f};
        tc.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        tc.scale = {20.0f, 0.2f, 20.0f};
        tc.flags = 1;
        registry.emplace<TransformComponent>(groundEntity_, tc);
        registry.emplace<WorldTransformComponent>(groundEntity_);
        registry.emplace<MeshComponent>(groundEntity_, MeshComponent{cubeMeshId});
        registry.emplace<MaterialComponent>(groundEntity_, MaterialComponent{groundMatId});
        registry.emplace<VisibleTag>(groundEntity_);
        registry.emplace<ShadowVisibleTag>(groundEntity_, ShadowVisibleTag{0xFF});

        RigidBodyComponent rb;
        rb.mass = 0.0f;
        rb.type = BodyType::Kinematic;
        rb.friction = 0.8f;
        rb.restitution = 0.1f;
        registry.emplace<RigidBodyComponent>(groundEntity_, rb);

        ColliderComponent col;
        col.shape = ColliderShape::Box;
        col.halfExtents = {10.0f, 0.1f, 10.0f};
        registry.emplace<ColliderComponent>(groundEntity_, col);
    }

    // ---- Plank (kinematic, rotated by keys) -------------------------------
    // 6 units long (X), 0.15 thick (Y), 1 deep (Z). Sits at y = 2.
    plankEntity_ = registry.createEntity();
    {
        TransformComponent tc{};
        tc.position = {0.0f, 2.0f, 0.0f};
        tc.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        tc.scale = {6.0f, 0.15f, 1.0f};
        tc.flags = 1;
        registry.emplace<TransformComponent>(plankEntity_, tc);
        registry.emplace<WorldTransformComponent>(plankEntity_);
        registry.emplace<MeshComponent>(plankEntity_, MeshComponent{cubeMeshId});
        registry.emplace<MaterialComponent>(plankEntity_, MaterialComponent{greyMatId});
        registry.emplace<VisibleTag>(plankEntity_);
        registry.emplace<ShadowVisibleTag>(plankEntity_, ShadowVisibleTag{0xFF});

        RigidBodyComponent rb;
        rb.mass = 0.0f;
        rb.type = BodyType::Kinematic;
        rb.friction = 0.9f;
        rb.restitution = 0.1f;
        registry.emplace<RigidBodyComponent>(plankEntity_, rb);

        ColliderComponent col;
        col.shape = ColliderShape::Box;
        col.halfExtents = {3.0f, 0.075f, 0.5f};
        registry.emplace<ColliderComponent>(plankEntity_, col);
    }

    // ---- Coin (small sphere at +X end) ------------------------------------
    spawnCoin(registry);

    // ---- Large ball (at -X end) -------------------------------------------
    ballEntity_ = registry.createEntity();
    {
        const float r = 0.55f;
        TransformComponent tc{};
        tc.position = {-2.4f, 2.7f, 0.0f};
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
        rb.mass = 10.0f;
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

    // ---- Camera -----------------------------------------------------------
    cam_.distance = 12.0f;
    cam_.pitch = 15.0f;
    cam_.target = {0.0f, 2.0f, 0.0f};
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

    // Swap the coin's mesh/material if it still exists.
    if (coinEntity_ != 0)
    {
        if (auto* mc = registry.get<MeshComponent>(coinEntity_))
            mc->mesh = coinMeshId_;
        if (auto* mm = registry.get<MaterialComponent>(coinEntity_))
            mm->material = coinMatId_;
    }

    assetsApplied_ = true;
}

void SampleGame::spawnCoin(Registry& registry)
{
    const float r = 0.2f;
    coinEntity_ = registry.createEntity();

    TransformComponent tc{};
    tc.position = {2.6f, 2.35f, 0.0f};
    // Stand the coin upright, then spin it 90° around Y.
    tc.rotation =
        glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    tc.scale = {r * 2.0f, r * 2.0f, r * 2.0f};
    tc.flags = 1;
    registry.emplace<TransformComponent>(coinEntity_, tc);
    registry.emplace<WorldTransformComponent>(coinEntity_);
    registry.emplace<MeshComponent>(coinEntity_, MeshComponent{coinMeshId_});
    registry.emplace<MaterialComponent>(coinEntity_, MaterialComponent{coinMatId_});
    registry.emplace<VisibleTag>(coinEntity_);
    registry.emplace<ShadowVisibleTag>(coinEntity_, ShadowVisibleTag{0xFF});

    RigidBodyComponent rb;
    rb.mass = 0.0f;
    rb.type = BodyType::Kinematic;
    rb.friction = 0.5f;
    rb.restitution = 0.3f;
    registry.emplace<RigidBodyComponent>(coinEntity_, rb);

    ColliderComponent col;
    col.shape = ColliderShape::Sphere;
    col.radius = r;
    col.isSensor = 1;  // overlap only — ball passes through, no impulse
    registry.emplace<ColliderComponent>(coinEntity_, col);
}

void SampleGame::onFixedUpdate(Engine& engine, Registry& registry, float fixedDt)
{
    // Apply directional force to the ball while movement keys are held.
    // Force is persistent per fixed step, so holding a direction accelerates.
    const auto& input = engine.inputState();
    glm::vec3 force{0.0f};
    constexpr float kForceMag = 45.0f;  // N; mass=10 → 4.5 m/s² of acceleration
    if (input.isKeyHeld(Key::Up) || input.isKeyHeld(Key::W))
        force.x += kForceMag;
    if (input.isKeyHeld(Key::Down) || input.isKeyHeld(Key::S))
        force.x -= kForceMag;
    if (input.isKeyHeld(Key::Left) || input.isKeyHeld(Key::A))
        force.z -= kForceMag;
    if (input.isKeyHeld(Key::Right) || input.isKeyHeld(Key::D))
        force.z += kForceMag;

    if (force.x != 0.0f || force.z != 0.0f)
    {
        if (auto* rb = registry.get<RigidBodyComponent>(ballEntity_); rb && rb->bodyID != ~0u)
            physics_.applyForce(rb->bodyID, {force.x, 0.0f, force.z});
    }

    physicsSys_.update(registry, physics_, fixedDt);

    // Detect ball-vs-coin contact via the physics engine's contact events.
    if (!coinCollected_)
    {
        for (const auto& evt : physics_.getContactBeginEvents())
        {
            const bool match =
                (evt.entityA == coinEntity_ && evt.entityB == ballEntity_) ||
                (evt.entityA == ballEntity_ && evt.entityB == coinEntity_);
            if (match)
            {
                if (beepClipId_ != 0)
                    audio_.play(beepClipId_, engine::audio::SoundCategory::SFX, 1.0f, false);
                registry.destroyEntity(coinEntity_);
                coinEntity_ = 0;
                coinCollected_ = true;
                break;
            }
        }
    }
}

void SampleGame::onUpdate(Engine& engine, Registry& registry, float dt)
{
    // Drain async asset uploads; apply GLB meshes once Ready.
    assets_.processUploads();
    if (!assetsApplied_)
        applyLoadedAssets(engine, registry);

    // Spin the coin around Y at 180°/s.
    coinSpinTime_ += dt;
    if (coinEntity_ != 0)
    {
        if (auto* tc = registry.get<TransformComponent>(coinEntity_))
        {
            const float spinDeg = coinSpinTime_ * 180.0f;
            tc->rotation =
                glm::angleAxis(glm::radians(spinDeg), glm::vec3(0.0f, 1.0f, 0.0f)) *
                glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
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
        resetBody(ballEntity_, {-2.4f, 2.7f, 0.0f});

        // Respawn the coin if it had been collected.
        if (coinCollected_)
        {
            spawnCoin(registry);
            coinCollected_ = false;
        }
    }
}

void SampleGame::onRender(Engine& engine)
{
    engine.renderer().beginFrameDirect();

    const auto W = engine.fbWidth();
    const auto H = engine.fbHeight();
    const float fbW = static_cast<float>(W);
    const float fbH = static_cast<float>(H);

    // Chase camera: behind and above the ball, looking at the coin.
    const glm::vec3 kCoinPos{2.6f, 2.35f, 0.0f};
    glm::vec3 ballPos{-2.4f, 2.7f, 0.0f};
    if (registry_)
    {
        if (auto* tc = registry_->get<TransformComponent>(ballEntity_))
            ballPos = glm::vec3(tc->position.x, tc->position.y, tc->position.z);
    }
    glm::vec3 toCoin = kCoinPos - ballPos;
    toCoin.y = 0.0f;
    float len = glm::length(toCoin);
    glm::vec3 fwd = (len > 1e-4f) ? (toCoin / len) : glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 anchor;
    if (coinCollected_)
    {
        // Freeze at a fixed spot along the initial ball→coin direction.
        fwd = glm::vec3(1.0f, 0.0f, 0.0f);
        anchor = kCoinPos - fwd * 3.0f;
    }
    else
    {
        // Camera always tracks the ball, however close to the coin.
        anchor = ballPos;
    }
    const glm::vec3 camPos = anchor - fwd * 5.5f + glm::vec3(0.0f, 3.0f, 0.0f);
    const glm::mat4 viewMat = glm::lookAt(camPos, kCoinPos, glm::vec3(0, 1, 0));
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
