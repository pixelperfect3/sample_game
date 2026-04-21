// Android entry point — implements the samaCreateGame() factory function.
// Sama's engine_android library provides android_main() which calls this.

#include "SampleGame.h"
#include "engine/game/IGame.h"

engine::game::IGame* samaCreateGame()
{
    return new SampleGame();
}
