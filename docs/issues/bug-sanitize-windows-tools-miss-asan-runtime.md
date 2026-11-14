---
created: 2026-11-13
---

# The Windows sanitize build cannot run its own shader compiler: sgl.exe does not find the ASan runtime

`uv run dev.py build --preset sanitize-clang` stops at the first SGL shader package with "'module:slug' does not compile, so nothing is generated from it:" and no diagnostic after it.
The sanitizer-built `sgl.exe` that `generate_shader_package.py` runs exits before printing anything: it cannot load `clang_rt.asan_dynamic-x86_64.dll`.
Putting the LLVM runtime directory on PATH does not reach it, since dev.py builds the environment ninja runs in; copying the DLL beside `sgl.exe` does.
Seen 2026-11-13 with LLVM 22; the generator also swallows the loader's message, which is why the error looks empty.
