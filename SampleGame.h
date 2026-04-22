#pragma once

#include "engine/assets/AssetHandle.h"
#include "engine/assets/AssetManager.h"
#include "engine/assets/GltfAsset.h"
#include "engine/assets/StdFileSystem.h"
#include <array>
#include <random>
#include <vector>

#include <glm/glm.hpp>

#include "engine/audio/SoLoudAudioEngine.h"
#include "engine/ecs/Entity.h"
#include "engine/game/IGame.h"
#include "engine/physics/JoltPhysicsEngine.h"
#include "engine/physics/PhysicsSystem.h"
#include "engine/rendering/IblResources.h"
#include "engine/rendering/systems/DrawCallBuildSystem.h"
#include "engine/threading/ThreadPool.h"
#include "engine/ui/BitmapFont.h"
#include "engine/ui/MsdfFont.h"
#include "engine/ui/UiCanvas.h"
#include "engine/ui/UiDrawList.h"
#include "engine/ui/UiRenderer.h"
#include <memory>

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
    static constexpr int kLevelCount = 2;

    void spawnCoin(engine::ecs::Registry& registry, int index);
    void spawnAllCoins(engine::ecs::Registry& registry);
    void spawnBall(engine::ecs::Registry& registry);
    void resetLevel(engine::ecs::Registry& registry);
    void loadLevel(engine::core::Engine& engine, engine::ecs::Registry& registry,
                   int level);
    void clearLevel(engine::ecs::Registry& registry);
    void spawnPlankLevel(engine::ecs::Registry& registry);
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
    uint32_t greyMatId_ = 0;
    uint32_t groundMatId_ = 0;

    // Entities
    engine::ecs::EntityID groundEntity_ = 0;
    engine::ecs::EntityID ballEntity_ = 0;
    std::array<engine::ecs::EntityID, kCoinCount> coinEntities_{};
    std::array<glm::vec3, kCoinCount> coinPositions_{};
    std::array<bool, kCoinCount> coinCollectedFlags_{};
    int coinsRemaining_ = kCoinCount;
    int coinCount_ = 1;
    int currentLevel_ = 0;
    bool showTitleScreen_ = true;
    glm::vec3 ballStartPos_{-2.4f, 2.7f, 0.0f};

    // Track level-geometry entities for cleanup
    std::vector<engine::ecs::EntityID> levelEntities_;

    // Accumulated time for the coin spin animation (shared across coins).
    float coinSpinTime_ = 0.0f;

    // Smoothed look-target (lerped toward nearest uncollected coin).
    glm::vec3 smoothedTargetPos_{0.0f, 0.55f, 0.0f};
    // Smoothed XZ direction from ball toward the target — used for both
    // camera orientation and the movement-key axis.
    glm::vec2 smoothedFwd_{0.0f, -1.0f};

    // Gyro calibration: gravity reading at level start = neutral tilt.
    float gyroBaseX_ = 0.0f;
    float gyroBaseY_ = 0.0f;
    bool gyroCalibrated_ = false;

    // Engine/Registry pointers used in onRender
    engine::core::Engine* engine_ = nullptr;
    engine::ecs::Registry* registry_ = nullptr;

    // UI (font + renderer + per-frame draw list for the HUD)
    engine::ui::MsdfFont msdfFont_;
    engine::ui::BitmapFont bitmapFont_;
    engine::ui::IFont* hudFont_ = nullptr;  // points to msdf or bitmap
    engine::ui::UiRenderer uiRenderer_;
    engine::ui::UiDrawList hudDrawList_;
    bool hudFontLoaded_ = false;

    // Retained-mode UI canvases (rebuilt lazily on state change).
    std::unique_ptr<engine::ui::UiCanvas> titleCanvas_;
    std::unique_ptr<engine::ui::UiCanvas> endLevelCanvas_;
    uint16_t canvasW_ = 0;
    uint16_t canvasH_ = 0;
    float canvasDpi_ = 1.0f;  // contentScale — used to scale pixel sizes for retina
    bool endLevelCanvasBuilt_ = false;
    bool endLevelCanvasHasNext_ = false;
    float prevMouseX_ = 0.f;
    float prevMouseY_ = 0.f;

    void buildTitleCanvas();
    void buildEndLevelCanvas(bool hasNextLevel);
    void dispatchMouseEvents(engine::core::Engine& engine, engine::ui::UiCanvas& canvas);
};
