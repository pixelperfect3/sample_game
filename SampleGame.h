#pragma once

#include "engine/assets/AssetHandle.h"
#include "engine/assets/AssetManager.h"
#include "engine/assets/GltfAsset.h"
#include "engine/assets/StdFileSystem.h"
#include "engine/audio/SoLoudAudioEngine.h"
#include "engine/core/OrbitCamera.h"
#include "engine/ecs/Entity.h"
#include "engine/game/IGame.h"
#include "engine/physics/JoltPhysicsEngine.h"
#include "engine/physics/PhysicsSystem.h"
#include "engine/rendering/IblResources.h"
#include "engine/rendering/systems/DrawCallBuildSystem.h"
#include "engine/threading/ThreadPool.h"

class SampleGame : public engine::game::IGame
{
public:
    SampleGame();
    ~SampleGame() override;

    void onInit(engine::core::Engine& engine, engine::ecs::Registry& registry) override;
    void onFixedUpdate(engine::core::Engine& engine, engine::ecs::Registry& registry,
                       float fixedDt) override;
    void onUpdate(engine::core::Engine& engine, engine::ecs::Registry& registry,
                  float dt) override;
    void onRender(engine::core::Engine& engine) override;
    void onShutdown(engine::core::Engine& engine, engine::ecs::Registry& registry) override;

private:
    void spawnCoin(engine::ecs::Registry& registry);
    void applyLoadedAssets(engine::core::Engine& engine, engine::ecs::Registry& registry);

    // Audio
    engine::audio::SoLoudAudioEngine audio_;
    uint32_t beepClipId_ = 0;

    // Physics / rendering
    engine::physics::JoltPhysicsEngine physics_;
    engine::physics::PhysicsSystem physicsSys_;
    engine::rendering::DrawCallBuildSystem drawCallSys_;
    engine::rendering::IblResources ibl_;
    engine::core::OrbitCamera cam_;

    // Asset loading
    engine::threading::ThreadPool threadPool_{1};
    engine::assets::StdFileSystem fileSystem_{"."};
    engine::assets::AssetManager assets_{threadPool_, fileSystem_};
    engine::assets::AssetHandle<engine::assets::GltfAsset> sphereHandle_{};
    engine::assets::AssetHandle<engine::assets::GltfAsset> coinHandle_{};
    bool assetsApplied_ = false;

    // Render resource IDs (cube = placeholder, replaced by GLB IDs when loaded)
    uint32_t cubeMeshId_ = 0;
    uint32_t coinMeshId_ = 0;
    uint32_t coinMatId_ = 0;
    uint32_t ballMeshId_ = 0;
    uint32_t ballMatId_ = 0;

    // Entities
    engine::ecs::EntityID groundEntity_ = 0;
    engine::ecs::EntityID plankEntity_ = 0;
    engine::ecs::EntityID coinEntity_ = 0;
    engine::ecs::EntityID ballEntity_ = 0;

    // Coin collected state
    bool coinCollected_ = false;

    // Accumulated time for the coin's spin animation.
    float coinSpinTime_ = 0.0f;

    // Registry pointer used in onRender
    engine::ecs::Registry* registry_ = nullptr;
};
