// Android entry point.
//
// We define `android_main` ourselves (instead of using engine_android's
// default in AndroidApp.cpp) so we can populate `EngineDesc` with the
// game's preferences — most importantly `enableGyro = true`, since gyro
// is opt-in as of sama's perf-audit work (the sensors burn standby
// power and most apps don't read them, so the engine default flipped
// to false).  See docs/PERF_AUDIT_2026-05-25.md item #P1.
//
// Linker note: our `main_android.cpp.o` is in the link list before
// `engine_android` is searched for `android_main`, so the engine's
// version inside AndroidApp.cpp.o is never pulled in.

#include <android_native_app_glue.h>

#include "engine/core/Engine.h"
#include "engine/game/GameRunner.h"
#include "SampleGame.h"

extern "C" void android_main(struct android_app* app)
{
    SampleGame game;
    engine::game::GameRunner runner(game);

    engine::core::EngineDesc desc;
    // Window dimensions come from the ANativeWindow in initAndroid;
    // the fields below are placeholders that initAndroid overrides.
    desc.enableGyro = true;       // game uses tilt control on Android
    desc.singleThreaded = false;  // multi-threaded bgfx (Sama default)

    runner.runAndroid(app, desc);
}
