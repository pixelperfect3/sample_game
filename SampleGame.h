#pragma once

#include "engine/assets/AssetHandle.h"
#include "engine/assets/AssetManager.h"
#include "engine/assets/GltfAsset.h"
#include "engine/assets/StdFileSystem.h"
#include <array>
#include <random>

#include <glm/glm.hpp>

#include "engine/audio/SoLoudAudioEngine.h"
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
    static constexpr int kCoinCount = 3;

    void spawnCoin(engine::ecs::Registry& registry, int index);
    void spawnAllCoins(engine::ecs::Registry& registry);
    void spawnFigureEightFloor(engine::ecs::Registry& registry, uint32_t meshId, uint32_t matId);
    void applyLoadedAssets(engine::core::Engine& engine, engine::ecs::Registry& registry);

    // Random coin placement
    std::mt19937 rng_{std::random_device{}()};

    // Audio
    engine::audio::SoLoudAudioEngine audio_;
    uint32_t beepClipId_ = 0;

    // Physics / rendering
    engine::physics::JoltPhysicsEngine physics_;
    engine::physics::PhysicsSystem physicsSys_;
    engine::rendering::DrawCallBuildSystem drawCallSys_;
    engine::rendering::IblResources ibl_;

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
    engine::ecs::EntityID ballEntity_ = 0;
    std::array<engine::ecs::EntityID, kCoinCount> coinEntities_{};
    std::array<glm::vec3, kCoinCount> coinPositions_{};
    std::array<bool, kCoinCount> coinCollectedFlags_{};
    int coinsRemaining_ = kCoinCount;

    // Accumulated time for the coin spin animation (shared across coins).
    float coinSpinTime_ = 0.0f;

    // Ball position snapshot at the moment the last coin is collected —
    // used to freeze the chase camera.
    glm::vec3 ballPosAtCollection_{0.0f};
    glm::vec3 lastCoinPos_{0.0f};

    // Registry pointer used in onRender
    engine::ecs::Registry* registry_ = nullptr;
};
