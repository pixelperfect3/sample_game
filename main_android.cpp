// ---------------------------------------------------------------------------
// Android entry point for Sample Game.
//
// On Android the GLFW-based Engine class is not available, so we integrate
// directly with Sama's Android platform layer (AndroidWindow, AndroidInput,
// AndroidGyro, AndroidFileSystem) and run a game loop that mirrors what
// GameRunner does on desktop.
//
// The Sama engine's runAndroidApp() provides a reference loop but does not
// yet integrate IGame callbacks, so we build our own NativeActivity loop
// here, calling SampleGame's IGame methods at the right points.
// ---------------------------------------------------------------------------

#include <android/configuration.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

#include <chrono>

#include "engine/core/Engine.h"
#include "engine/game/GameRunner.h"
#include "engine/platform/android/AndroidFileSystem.h"
#include "engine/platform/android/AndroidGyro.h"
#include "engine/platform/android/AndroidInput.h"
#include "engine/platform/android/AndroidWindow.h"
#include "engine/scene/TransformSystem.h"

#include "SampleGame.h"

#define LOGI(...) \
    __android_log_print(ANDROID_LOG_INFO, "SampleGame", __VA_ARGS__)
#define LOGE(...) \
    __android_log_print(ANDROID_LOG_ERROR, "SampleGame", __VA_ARGS__)

// ---------------------------------------------------------------------------
// App state shared between the NativeActivity callbacks and the main loop.
// ---------------------------------------------------------------------------
namespace
{

struct AppState
{
    engine::platform::AndroidWindow window;
    engine::platform::AndroidInput input;
    engine::platform::AndroidGyro gyro;
    engine::input::InputState inputState;
    std::unique_ptr<engine::platform::AndroidFileSystem> fileSystem;

    bool focused = false;
    bool bgfxInitialized = false;
};

static void handleAppCmd(struct android_app* app, int32_t cmd)
{
    auto* state = static_cast<AppState*>(app->userData);
    if (!state) return;

    switch (cmd)
    {
        case APP_CMD_INIT_WINDOW:
            LOGI("APP_CMD_INIT_WINDOW");
            if (app->window)
            {
                state->window.setNativeWindow(app->window);
                AConfiguration* config = AConfiguration_new();
                AConfiguration_fromAssetManager(
                    config, app->activity->assetManager);
                int32_t density = AConfiguration_getDensity(config);
                AConfiguration_delete(config);
                if (density > 0) state->window.setDensity(density);
            }
            break;

        case APP_CMD_TERM_WINDOW:
            LOGI("APP_CMD_TERM_WINDOW");
            if (state->bgfxInitialized)
            {
                bgfx::shutdown();
                state->bgfxInitialized = false;
            }
            state->window.clearNativeWindow();
            break;

        case APP_CMD_GAINED_FOCUS:
            state->focused = true;
            state->gyro.setEnabled(true);
            break;

        case APP_CMD_LOST_FOCUS:
            state->focused = false;
            state->gyro.setEnabled(false);
            break;

        case APP_CMD_CONFIG_CHANGED:
            state->window.updateSize();
            break;
    }
}

static int32_t handleInput(struct android_app* app, AInputEvent* event)
{
    auto* state = static_cast<AppState*>(app->userData);
    if (!state) return 0;
    return state->input.handleInputEvent(event, state->inputState);
}

static bool initBgfx(AppState& state)
{
    bgfx::PlatformData pd{};
    pd.nwh = state.window.nativeWindow();
    bgfx::setPlatformData(pd);

    bgfx::Init init;
    init.type = bgfx::RendererType::Vulkan;
    init.resolution.width = state.window.width();
    init.resolution.height = state.window.height();
    init.resolution.reset = BGFX_RESET_VSYNC;

    if (!bgfx::init(init))
    {
        LOGE("bgfx::init() failed");
        return false;
    }

    bgfx::setViewClear(
        0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x443355FF, 1.0f, 0);
    bgfx::setViewRect(
        0, 0, 0, state.window.width(), state.window.height());

    LOGI("bgfx initialized: %ux%u Vulkan",
         state.window.width(), state.window.height());
    state.bgfxInitialized = true;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// NativeActivity entry point.
// ---------------------------------------------------------------------------
void android_main(struct android_app* app)
{
    LOGI("SampleGame — android_main starting");

    AppState state;
    app->userData = &state;
    app->onAppCmd = handleAppCmd;
    app->onInputEvent = handleInput;

    state.fileSystem = std::make_unique<engine::platform::AndroidFileSystem>(
        app->activity->assetManager);

    // Initialise the gyroscope/accelerometer sensor.
    ALooper* looper = ALooper_forThread();
    if (looper)
        state.gyro.init(looper);

    // --- Game setup ---
    // The Engine object is GLFW-based and cannot be used on Android.
    // We provide a minimal shim here.  The SampleGame IGame callbacks
    // that depend on Engine (inputState, renderer, etc.) will need a
    // future Engine-Android implementation.  For now we set up bgfx
    // and run the Android event loop, demonstrating the entry-point
    // wiring and gyro integration.

    using Clock = std::chrono::steady_clock;
    auto lastTime = Clock::now();

    while (!app->destroyRequested)
    {
        // Poll Android events.
        int events;
        struct android_poll_source* source;
        int timeout = (state.window.isReady() && state.focused) ? 0 : -1;

        while (ALooper_pollAll(
                   timeout, nullptr, &events,
                   reinterpret_cast<void**>(&source)) >= 0)
        {
            if (source) source->process(app, source);
            if (app->destroyRequested) break;
        }

        if (app->destroyRequested) break;

        // Init bgfx when the window becomes available.
        if (state.window.isReady() && !state.bgfxInitialized)
        {
            if (!initBgfx(state))
            {
                LOGE("Failed to initialise bgfx — exiting");
                break;
            }
        }

        // Render a frame.
        if (state.bgfxInitialized && state.window.isReady() && state.focused)
        {
            // Update gyro/accelerometer sensor data.
            state.gyro.update(state.inputState);

            // Compute delta time.
            auto now = Clock::now();
            float dt = std::chrono::duration<float>(now - lastTime).count();
            lastTime = now;
            if (dt > 0.25f) dt = 0.25f;  // clamp large spikes

            // TODO: Once Engine supports Android, create a SampleGame
            // instance and call its IGame callbacks here:
            //   game.onFixedUpdate(engine, registry, fixedDt);
            //   game.onUpdate(engine, registry, dt);
            //   game.onRender(engine);

            bgfx::touch(0);
            bgfx::frame();
        }

        state.input.endFrame(state.inputState);
    }

    // Cleanup.
    state.gyro.shutdown();
    if (state.bgfxInitialized)
    {
        bgfx::shutdown();
        state.bgfxInitialized = false;
    }
    state.fileSystem.reset();
    app->userData = nullptr;

    LOGI("SampleGame — android_main exiting");
}
