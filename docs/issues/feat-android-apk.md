---
created: 2026-11-05
priority: normal
tags: [xr, android, dev.py]
parent: epic-xr
claimed-by: Claude (for Philip)
claimed-at: 2026-11-06T13:21Z
---

# A gradle-free Android APK, deployed and launched by `dev.py`

The NDK presets build, but nothing packages an APK, so nothing runs on a Quest.
`epic-xr` reaches the Quest over Link first; this is what runs it standalone, and it is useful beyond XR: any Android example or app needs it.

## Wanted

- An APK step in CMake from the SDK's build tools: `aapt2` for the manifest, `zipalign`, `apksigner` with a debug key.
- `NativeActivity` as the entry point, so an app is C++ only; nexus's `APP` becomes `android_main` there.
- `dev.py` installs and launches over `adb`, and streams the app's log back.
- Done when a triangle draws on the Quest 3's 2D panel through vulkan.

## Status (2026-11-06)

Packaging exists: `sc_add_android_apk` makes a signed, aligned APK with no gradle, and `android-hello` is one, which fills its window with no GPU code.
`dev.py android run` builds, installs, starts it and streams its log.
None of this has met a device, so install and launch are untested, and `aapt2 dump badging` and `apksigner verify` are what it has passed.

Left, and it needs a device:

- Run `android-hello` on the Quest's 2D panel.
- A vulkan surface for Android: sg's backend has none yet, so the triangle waits for that.
- `NativeActivity` with a real loop, `android_main` and nexus's `APP` there.

## Known

- Decided against gradle: it would be the repo's first JVM toolchain.
- `dev.py install android-sdk` provides `aapt2`, `zipalign`, `apksigner` and `adb` with no Java.
- `apksigner` and `keytool` are Java tools, so signing needs a Java runtime; without one the APK stays unsigned and will not install.
- SDL3 on Android brings its own Java activity, which competes for the entry point; an XR app needs no window.
- A test binary still runs as a plain executable under `adb shell`, with no APK.
- `session::availability_of` never reports `needs_permission` or `denied` until this exists: Android's `USE_SCENE` grant is what feeds them.
