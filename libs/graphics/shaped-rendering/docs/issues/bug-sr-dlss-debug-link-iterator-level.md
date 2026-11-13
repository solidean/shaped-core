---
created: 2026-11-13
refs:
  - file: extern/dlss/fetch-dlss.py
---

# With DLSS fetched, a Debug build fails to link: NVIDIA's debug library and FSR disagree on _ITERATOR_DEBUG_LEVEL

`sanitize-clang`, a Debug build, fails to link `shaped-viewer-test` once `extern/dlss/.install` exists.
lld-link reports `/failifmismatch: mismatch detected for '_ITERATOR_DEBUG_LEVEL'`: `ffx_fsr3upscaler.cpp.obj` in shaped-rendering has 0, `nvsdk_ngx_lib_windows.obj` in `nvsdk_ngx_d_dbg.lib` has 2.
Seen 2026-11-13 with DLSS 310.9.1; the release and relwithdebinfo presets link, and every Debug preset presumably fails the same way.
Either the Debug presets link the release `nvsdk_ngx_d.lib`, or FSR's sources stop forcing level 0.
