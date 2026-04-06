#include "engine/core/Engine.h"
#include "engine/game/IGame.h"
#include "engine/game/GameRunner.h"
#include "SampleGame.h"

#include <unistd.h>
#import <Foundation/Foundation.h>

int main()
{
    // Set cwd to the app bundle's Resources directory so relative asset
    // paths (assets/, levels/) resolve correctly inside a .app bundle.
    // Only chdir when running as a .app — bare executable keeps project root cwd.
    NSString* bundlePath = [[NSBundle mainBundle] bundlePath];
    if (bundlePath && [bundlePath hasSuffix:@".app"])
    {
        NSString* resourcePath = [[NSBundle mainBundle] resourcePath];
        if (resourcePath)
            chdir([resourcePath UTF8String]);
    }

    SampleGame game;
    engine::game::GameRunner runner(game);
    return runner.run(engine::core::EngineDesc{
        .windowWidth = 1920,
        .windowHeight = 1080,
        .windowTitle = "My Game"
    });
}
