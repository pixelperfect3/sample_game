#include "engine/core/Engine.h"
#include "engine/game/IGame.h"
#include "engine/game/GameRunner.h"
#include "SampleGame.h"


int main()
{
    SampleGame game;
    engine::game::GameRunner runner(game);
    return runner.run(engine::core::EngineDesc{
        .windowWidth = 1920,
        .windowHeight = 1080,
        .windowTitle = "My Game"
    });
}
