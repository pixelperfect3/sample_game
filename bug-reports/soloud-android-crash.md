# Crash: `SoLoud::mapResampleBuffers_internal` SIGSEGV on Android AAudio callback during init

**Severity:** crash on launch (Android, any game using `engine_audio`)
**Component:** `engine_audio` (SoLoud + miniaudio AAudio backend)
**Status:** open
**First seen:** sama `9b4f123` (2026-05-22); previous working commit on this device: `1bfe1ab`
**Reporter:** `sample_game` integration

---

## TL;DR

On Android, `audio_.init()` returns success, the engine logs `Audio: SoLoud (miniaudio) initialised`, then within a few hundred ms the AAudio playback callback thread crashes with `SIGSEGV` at `0x0` inside `SoLoud::Soloud::mapResampleBuffers_internal()`. No game-side audio call is needed to trigger it — the callback thread starts running before SoLoud's mix state is fully wired up, and racing against init dereferences a null buffer.

Workaround in `sample_game`: `constexpr bool kEnableAudio = false;` gating `audio_.init()`. Game runs fine with audio disabled.

## Environment

| | |
|---|---|
| Sama commit | `9b4f123ea18c3b3d6dc4545e2e34c7839fff41a0` (main, 2026-05-22) |
| Device | Google Pixel 9 (Tensor G4, Mali-G715) |
| OS | Android 16 |
| ABI | arm64-v8a |
| NDK | 26.1.10909125 |
| Build type | Release |
| `SAMA_ANDROID` | ON |
| Graphics backend | bgfx Vulkan |
| Audio backend | SoLoud → miniaudio → AAudio |

Last sama commit on which the same `sample_game` ran cleanly on this device: `1bfe1ab` (a few weeks ago, pre-Phase-7 / pre-Opus work).

## Repro

`sample_game` (commit [`d554290`](https://github.com/pixelperfect3/sample_game/commit/d554290)) → `./android/build_apk.sh --install`. Default `SampleGame::onInit()` calls:

```cpp
if (audio_.init())
{
    const auto bytes = readFileBytes("assets/beep.wav");
    if (!bytes.empty())
        beepClipId_ = audio_.loadClip(bytes.data(), bytes.size());
}
```

No `audio_.play(...)` call is needed — the crash fires before the game's first frame. Crash is **100 % reproducible** on this device.

Minimal repro outside `sample_game`: instantiate `engine::audio::SoLoudAudioEngine`, call `init()`, idle. Expect crash within ~700 ms.

## Logcat — leading events

```
00:58:06.139  I SamaEngine: Audio: SoLoud (miniaudio) initialised
00:58:06.307  I SamaEngine: Sama Engine — Android init complete (1080x2424)
00:58:06.696  W SampleGame: extractAllAssets: not found in APK: project.json
00:58:06.696  I SampleGame: Extracted 6/7 assets to /data/data/.../cache
00:58:06.713  D PlayerBase::PlayerBase()
00:58:06.728  D PlayerBase::PlayerBase()
00:58:06.728  D PlayerBase::~PlayerBase()
00:58:06.786  F libc    : Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR),
                          fault addr 0x0 in tid 6193 (AudioTrack), pid 6163
```

The crashing thread is named `AudioTrack` (AAudio's playback callback), not the game thread.

## Backtrace

```
#00 pc 0x000000000027f310  libsample_game.so   SoLoud::Soloud::mapResampleBuffers_internal()+120
#01 pc 0x000000000027f918  libsample_game.so   SoLoud::Soloud::mix_internal(unsigned int)+516
#02 pc 0x000000000027fcb4  libsample_game.so   SoLoud::Soloud::mix(float*, unsigned int)+32
#03 pc 0x00000000002f9c70  libsample_game.so   ma_device__on_data(ma_device*, void*, void const*, unsigned int)+600
#04 pc 0x00000000002f98ac  libsample_game.so   ma_device__handle_data_callback(...)+336
#05 pc 0x00000000002c1a08  libsample_game.so   ma_device__read_frames_from_client(...)+492
#06 pc 0x00000000002c13ec  libsample_game.so   ma_device_handle_backend_data_callback+644
#07 pc 0x00000000002f6bb8  libsample_game.so   ma_stream_data_callback_playback__aaudio(ma_AAudioStream_t**, void*, void*, int)+28
#08 pc 0x000000000000c514  libaaudio_internal_core.so      aaudio::AudioStream::dataCallbackInternal(void*, int)+100
#09 pc 0x000000000001127c  libaaudio_internal_core.so      aaudio::AudioStream::maybeCallDataCallback(void*, int)+108
#10 pc 0x000000000000e96c  libaaudio_internal_legacy.so    aaudio::AudioStreamLegacy::callDataCallbackFrames(...)+220
#11 pc 0x000000000000d7b0  libaaudio_internal_legacy.so    aaudio::AudioStreamLegacy::onMoreData(...)+608
#12 pc 0x00000000000badcc  libaudioclient.so               android::AudioTrack::processAudioBuffer()+2716
```

## Suspected cause

A race during `SoLoudAudioEngine::init()`:

1. `Soloud::init()` calls miniaudio's `ma_device_init()`.
2. AAudio creates a `MMAP_LEGACY` stream which **starts running its callback thread immediately**, before `ma_device_init()` returns.
3. Either:
   - SoLoud's `Soloud::init()` body hasn't yet allocated `mResampleBufferCount` / `mResampleData[]` arrays — `mapResampleBuffers_internal` then dereferences `mResampleData[bufferIdx]` while the array is still null, **or**
   - The buffer count was set in a prior frame to a value the callback thread caches, then resized non-atomically.

Faulting address `0x0` and the function name (`mapResampleBuffers`) strongly suggest dereferencing the resample-buffer pointer table before it's populated.

The mix internal stack with no active voices yet (we never called `play()`) — combined with the AAudio callback thread arriving fast on Android 14+ devices — is what makes this so reproducible on Pixel 9. On slower phones the same code path may "win the race" by accident.

## What we ruled out

- **Not a game-side issue.** Disabling the `loadClip` call alone still crashes; only gating the whole `audio_.init()` prevents it. No game thread is on the stack.
- **Not bgfx / Vulkan.** Renderer comes up cleanly (shaders all load, view setup succeeds, engine logs "Android init complete (1080x2424)") and crash is in a separate thread.
- **Not asset extraction.** Crash fires regardless of whether `beep.wav` is in the APK or not, before any `play()` is issued.
- **Not the missing project.json** the log warns about — that's our own asset-list bug, irrelevant to the audio path.

## Suggested investigation direction

Likely the fix lives in `SoLoudAudioEngine::init` or whatever wraps `Soloud::init`. Two specific hypotheses, in order of likelihood:

1. **Reorder `ma_device_start()` after SoLoud is fully ready.** miniaudio supports `ma_device_init(...)` → user does setup → `ma_device_start(...)`. If `engine_audio` is doing one-call `ma_device_init_ex` with `mDeviceState = MA_DEVICE_STATE_STARTED`, switch to explicit `init`-then-`start` so the callback can't fire before `Soloud::mResampleData` is allocated.

2. **Pin the callback to wait for a barrier.** Have `SoLoudAudioEngine::init` end with a memory-barrier + atomic `mInitialized = true` flag and bail in `Soloud::mix` if not set. Cheaper than reordering; matches what some other SoLoud Android integrations do.

A quick test that would confirm hypothesis (1): insert `usleep(50000)` between `SoLoud::init()` and the first time the AAudio device is allowed to start. If the crash disappears, it's the init race.

## Reference: working workaround in `sample_game`

```cpp
// Workaround: latest Sama's SoLoud/miniaudio Android playback path
// crashes with SIGSEGV in mapResampleBuffers_internal (race in the
// AAudio callback during init).  Skip audio init while investigating.
constexpr bool kEnableAudio = false;
// ...
if (kEnableAudio && audio_.init())
{
    // ... load clips ...
}
```

This is fine for our perf investigation but is obviously not a real fix. Once a sama-side fix lands we'll flip the constant back and verify.

## Acceptance test

A fix should: `SampleGame.cpp`'s `kEnableAudio = true`; build APK; install; run for ≥ 10 seconds idle on Pixel 9 → no crash, `audio_.play(beepClipId_, ...)` on coin pickup plays the beep. Equivalent test on Pixel 6 / Galaxy S24 / emulator would catch regressions on different audio backends.
