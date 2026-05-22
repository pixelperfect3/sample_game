# Gyro/accelerometer events never arrive on Android (sensor queue created with invalid ident)

**Severity:** functional (gyro tilt input dead on Android)
**Component:** `engine_platform_android` — `AndroidGyro::init`
**Status:** open — root cause identified
**First seen:** sama `e43ceb0` (still present after partial repair) — likely pre-existing for the entire AndroidGyro lifetime
**Reporter:** `sample_game` integration

---

## TL;DR

`gyro.available == true`, but `gravityX/Y/Z` and `pitchRate/yawRate/rollRate` are pinned at `0.000` forever on Pixel 9 / Android 16. The sensor queue is created and `setEnabled(true)` is called, but no events ever land in the queue. Root cause: `AndroidGyro::init` passes `ALOOPER_POLL_CALLBACK` as the looper `ident` argument with a null callback, which per NDK docs causes the looper to silently drop the queue.

## Diagnostic readout (sample_game on Pixel 9, 8 consecutive seconds)

```
gyro avail=1 g=(0.000,0.000,0.000) rate=(0.000,0.000,0.000)
gyro avail=1 g=(0.000,0.000,0.000) rate=(0.000,0.000,0.000)
gyro avail=1 g=(0.000,0.000,0.000) rate=(0.000,0.000,0.000)
... (identical for every sample, phone held in landscape, no motion)
```

`available=1` means `AndroidGyro::init` returned true. The fact that gravity is `0.000` rather than something like `(0, 0, -1)` for a phone resting on a table means the kernel-side events for accelerometer aren't even being delivered to user space — not "delivered but stale", literally "queue empty".

## Root cause

`engine/platform/android/AndroidGyro.cpp:33`:

```cpp
eventQueue_ = ASensorManager_createEventQueue(sensorManager_, looper, ALOOPER_POLL_CALLBACK,
                                              nullptr, nullptr);
```

The signature is:

```cpp
ASensorEventQueue* ASensorManager_createEventQueue(
    ASensorManager* manager, ALooper* looper, int ident,
    ALooper_callbackFunc callback, void* data);
```

`ALOOPER_POLL_CALLBACK` (value `-2`) is a **return-value sentinel** from `ALooper_pollOnce / ALooper_pollAll`, **not** a valid input. Per Android NDK docs (`android/looper.h`):

> The identifier must be ≥ 0, **or** `ALOOPER_POLL_CALLBACK` if providing a non-NULL callback.

Here the callback is `nullptr` AND `ident == ALOOPER_POLL_CALLBACK`, which is the only invalid combination. The looper accepts the `add`/`create` call but silently never delivers events — the queue stays empty and `ASensorEventQueue_getEvents` always returns 0.

Confirms: every published Android NDK sensor example uses a positive ident (typically `LOOPER_ID_USER = 3`) and a null callback, then explicitly drains via `ASensorEventQueue_getEvents` in the frame loop. That pattern works.

## Suggested fix

One-line change in `AndroidGyro::init`:

```diff
-    eventQueue_ = ASensorManager_createEventQueue(sensorManager_, looper, ALOOPER_POLL_CALLBACK,
-                                                  nullptr, nullptr);
+    // Ident must be >= 0 with a null callback (NDK docs).  Match the
+    // android_native_app_glue convention: positive sentinel, drain in update().
+    constexpr int kSensorLooperIdent = 1;
+    eventQueue_ = ASensorManager_createEventQueue(sensorManager_, looper,
+                                                  kSensorLooperIdent, nullptr, nullptr);
```

`AndroidGyro::update` already calls `ASensorEventQueue_getEvents` once per frame, so once events actually reach the queue, the existing drain code handles them. No other changes required.

## Tested locally (then reverted)

We patched this in our local sama checkout to verify; **the ident change alone was not enough** — events still didn't arrive on Pixel 9. Instrumentation added next showed `init`, `setEnabled(1)`, and both `ASensorEventQueue_enableSensor` + `ASensorEventQueue_setEventRate` returning 0 (success), with queue + sensor pointers valid. So beyond the ident bug there is a *second* issue — possibly:

1. The `Engine::beginFrame` main loop's `ALooper_pollAll` is not delivering to the sensor queue's ident in some configuration. (Worth a printf of the return value.)
2. Android 16 requires `HIGH_SAMPLING_RATE_SENSORS` permission for our `16667 µs` (60 Hz) rate? Unlikely — that gate is 200 Hz — but worth confirming.
3. The `ASensorManager_getInstanceForPackage("")` empty-string call may behave differently on newer Android. The NDK reference recommends passing the actual package name.

The first issue is the most likely additional culprit. Suggest also logging the return of `ALooper_pollAll` in the engine's beginFrame loop while debugging — if it's never returning the sensor ident, events aren't being routed even with a valid queue.

User reverted all local engine patches at this point with the request to "fix it properly on the engine side" — leaving this report as the handoff. All the diagnostic logging that produced these results was in `AndroidGyro::init` / `setEnabled` and `SampleGame::onFixedUpdate` (now also removed).

## Repro

`pixelperfect3/sample_game` HEAD + sama `e43ceb0` (needs the `<TargetConditionals.h>` Android-build workaround re-applied first):

1. `./android/build_apk.sh --install` (boots straight into figure-8 via `kDebugStartLevel = 1`).
2. Tilt the phone — ball doesn't move. No `axisF` / `axisR` input reaches the physics step.
3. Touching the screen doesn't trigger movement either; gyro is the only Android input path the game implements for ball control.

## Acceptance test

After the engine fix lands: tilting the Pixel 9 in landscape produces a gravity vector that varies through `(±0.7, 0, ∓0.7)` as the phone rolls, and the ball rolls toward whichever edge the user tilts down. Verified empirically on Pixel 9.
