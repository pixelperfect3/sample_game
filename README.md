# sample_game

A tiny physics puzzle built on the [Sama engine](https://github.com/pixelperfect3/sama).

The floor is shaped like the number 8 — two rings with hollow centres.
Push a red ball around the figure-8 to reach a yellow coin placed at a
random spot on the other ring. The coin vanishes on contact.

## Controls

| Key          | Action              |
|--------------|---------------------|
| Up / W       | Push ball forward   |
| Down / S     | Push ball back      |
| Left / A     | Push ball left      |
| Right / D    | Push ball right     |
| R            | Reset scene         |

Hold a direction — the ball accumulates speed.

## Build

Requires CMake ≥ 3.20, a C++20 compiler, and macOS (Metal backend). The first
build downloads Sama and all its dependencies (bgfx, glslang, Jolt, glfw,
SoLoud, ...) via CMake `FetchContent` — expect several minutes.

```sh
cmake -B build
cmake --build build --target sample_game -j$(sysctl -n hw.ncpu)
./build/sample_game
```

Run from the project root so `assets/` is found on the relative path.

## Layout

```
SampleGame.h / .cpp    game logic (IGame implementation)
main.mm                entry point — creates GameRunner
tools/make_primitives.py  generates assets/models/{sphere,coin}.glb
assets/beep.wav        collision sound
assets/models/*.glb    ball and coin meshes
```

## Regenerating meshes

The sphere and coin models are generated procedurally:

```sh
python3 tools/make_primitives.py
```

## Engine abstractions used

- `IGame` + `GameRunner` for the frame lifecycle
- `JoltPhysicsEngine` + `PhysicsSystem` + `ColliderComponent` (sphere, sensor)
- `SoLoudAudioEngine` for playback, triggered by physics contact events
- `AssetManager` + `GltfLoader` for async glTF loading
- `IblResources` for image-based lighting, `RenderPass` + `PbrFrameParams` for rendering

See `CLAUDE.md` for AI-assisted development notes.
