# Testing shaped-xr without a headset

`shaped-xr-test` holds two kinds of test.
The conversions and mappings need no runtime and always run.
`tests/session-test.cc` drives real sessions, and skips unless Monado's simulated headset is selected, because on any other runtime a session would start on someone's head.

## What Monado covers

It proves instance and system creation, the vulkan and dx12 bindings, a session reaching ready, the frame loop, and view instancing into the runtime's images.
It proves nothing about Meta's AR extensions, which it does not have, and it composes nothing: its null compositor renders no image, so a test only reads back what it drew itself.
The null compositor paces at 20 Hz, so each frame a test waits for costs 50 ms.
The OpenXR loader holds one instance per process, so these tests take `exclusive("openxr")` and run one at a time.

## Running it

Monado has no prebuilt Windows runtime, so it is built from source, and three environment variables select it:

```bash
export XR_RUNTIME_JSON='<monado-build>\openxr_monado-dev.json'
export XRT_COMPOSITOR_NULL=1
export SIMULATED_ENABLE=1
uv run dev.py test shaped-xr-test
```

`xr-cube` and `xr-probe` run against it the same way.

## Building Monado on Windows

Monado needs its pinned vcpkg baseline, the Vulkan SDK and MSVC, from a shell with `vcvars64` and `VULKAN_SDK` set:

```bat
cmake -S monado -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
  "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc /Zc:__cplusplus" ^
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>\scripts\buildsystems\vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows -DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON ^
  -DXRT_FEATURE_SERVICE=OFF -DBUILD_TESTING=OFF -DBUILD_DOC=OFF
ninja -C build
```

Two of those are not optional, as of Monado 2b7f5cb on MSVC 14.51:

- **`/Zc:__cplusplus`:** `u_file.cpp` tests `__cplusplus` for `<filesystem>`, and MSVC reports C++98 without the flag, so it falls back to a header MSVC no longer ships.
- **The main compositor stays on:** `targets/openxr/CMakeLists.txt` links `comp_main` unconditionally, so a null-only build fails to link.
  `XRT_COMPOSITOR_NULL` selects the null compositor at run time anyway.
