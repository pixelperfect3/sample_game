// Android entry point — mirrors main.mm but uses GameRunner::runAndroid()
// which handles the NativeActivity lifecycle, bgfx init, gyro/touch input,
// and the frame loop. SampleGame's IGame callbacks run identically to desktop.

#include <android_native_app_glue.h>

#include "engine/core/Engine.h"
#include "engine/game/GameRunner.h"
#include "SampleGame.h"

void android_main(struct android_app* app)
{
    SampleGame game;
    engine::game::GameRunner runner(game);
    runner.runAndroid(app, engine::core::EngineDesc{
        .windowWidth = 1920,
        .windowHeight = 1080,
        .windowTitle = "Sample Game"
    });
}
