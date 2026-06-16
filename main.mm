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
    engine::core::EngineDesc desc;
    desc.windowWidth = 1920;
    desc.windowHeight = 1080;
    desc.windowTitle = "My Game";
    desc.enableGyro = true;        // for cross-platform parity with Android
    desc.singleThreaded = false;   // multi-threaded bgfx (Sama default)
    return runner.run(desc);
}
