# Third-party licenses

Every external dependency shaped-core builds against, and the license each is under.
Our own license is in [shaped-core.txt](shaped-core.txt), so this directory is a complete bundle rather than everything-except-ours.

**Generated — do not edit by hand.**
`uv run dev.py deps licenses` regenerates it from the `extern/<dep>/dependency.yml` manifests, and `--check` verifies it has not drifted.

| Dependency | Version | License | Used by | Text |
|---|---|---|---|---|
| [BLAKE3](https://github.com/BLAKE3-team/BLAKE3) | 1.8.6 | `CC0-1.0 OR Apache-2.0 OR Apache-2.0 WITH LLVM-exception` | clean-core — cc::blake3 / cc::hash256 (the cryptographic hash) | [blake3-cc0.txt](blake3-cc0.txt), [blake3-a2.txt](blake3-a2.txt), [blake3-a2llvm.txt](blake3-a2llvm.txt) |
| [DXC](https://github.com/microsoft/DirectXShaderCompiler) | v1.9.2602.24 | `LicenseRef-Microsoft-DXC-Binary` | shaped-shader-compiler-dxc — HLSL to sg::compiled_shader | [dxc.txt](dxc.txt) |
| [Dear ImGui](https://github.com/ocornut/imgui) | v1.92.8-docking | `MIT` | shaped-rendering — sr::imgui_context and sr::imgui_routine | [dear-imgui.txt](dear-imgui.txt) |
| [ImPlot](https://github.com/epezent/implot) | d65a2bef53d3 | `MIT` | shaped-rendering — plotting inside the imgui bundle | [implot.txt](implot.txt) |
| [ImGuizmo](https://github.com/cedricguillemet/ImGuizmo) | dc25afb98bc3 | `MIT` | shaped-rendering — manipulation gizmos inside the imgui bundle | [imguizmo.txt](imguizmo.txt) |
| [libspng](https://libspng.org) | v0.7.4 | `BSD-2-Clause AND libpng-2.0` | babel-serializer — the PNG codec behind babel::png (vendored; the switch off stb is a separate change, so nothing links it yet) | [libspng.txt](libspng.txt), [libspng-libpng.txt](libspng-libpng.txt) |
| [LZ4](https://github.com/lz4/lz4) | v1.10.0 | `BSD-2-Clause` | clean-core — cc::compress / cc::decompress, the LZ4 algorithm | [lz4.txt](lz4.txt) |
| [mimalloc](https://github.com/microsoft/mimalloc) | v3.3.2 | `MIT` | clean-core — the default general memory resource | [mimalloc.txt](mimalloc.txt) |
| [SDL3](https://github.com/libsdl-org/SDL) | release-3.4.12 | `Zlib` | shaped-rendering — sr::window_system and sr::window | [sdl3.txt](sdl3.txt) |
| [SQLite](https://sqlite.org) | 3.53.3 | `blessing` | babel-serializer — the babel::sqlite engine wrapper | [sqlite.txt](sqlite.txt) |
| [stb](https://github.com/nothings/stb) | 31c1ad374564 | `MIT OR Unlicense` | babel-serializer — the PNG and JPEG codecs behind babel::image | [stb.txt](stb.txt) |
| [xxHash](https://github.com/Cyan4973/xxHash) | v0.8.3 | `BSD-2-Clause` | clean-core — cc::hash128 (the XXH3 128-bit hash) | [xxhash.txt](xxhash.txt) |
| [zlib](https://zlib.net) | v1.3.1 | `Zlib` | clean-core — cc::compress / cc::decompress, the Deflate algorithm | [zlib.txt](zlib.txt) |
| [Zstandard](https://github.com/facebook/zstd) | v1.5.7 | `BSD-3-Clause` | clean-core — cc::compress / cc::decompress, the Zstandard algorithm | [zstandard.txt](zstandard.txt) |
| [Zydis](https://github.com/zyantific/zydis) | v4.1.1 | `MIT` | instruction-tracer — decoding what optimized code actually executed | [zydis.txt](zydis.txt) |
| [Zycore](https://github.com/zyantific/zycore-c) | 0b2432ced088 | `MIT` | instruction-tracer — folded into the amalgamated Zydis | [zycore.txt](zycore.txt) |
| shaped-core | — | `MIT` | this repository | [shaped-core.txt](shaped-core.txt) |

The allowlist these are gated against is [tools/deps/license-policy.yml](../../tools/deps/license-policy.yml),
which `dev.py check` enforces so a version bump cannot quietly introduce a copyleft dependency.
