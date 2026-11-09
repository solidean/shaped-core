---
created: 2026-11-06
priority: someday
refs:
  - file: CMakePresets.json
  - file: tools/dev/lib/pipeline/sites.py
claimed-by: Claude (for Julius Nehring-Wirxel)
claimed-at: 2026-11-09T16:22Z
---

# The MSVC, GCC, macOS, iOS and Android development presets still build at SC_SOURCE_SITES=FULL

The clang development presets on Windows and Linux build at OPAQUE (docs/platforms.md, 'Source sites'); the others were left at FULL, unverified below it.

What moving each to OPAQUE needs checked, none of it measured yet:

* cl.exe captures a site with a call, not a `lea`, so a hot assert costs more; and its Debug link is incremental, so a test body's address may be a jump thunk the symbolizer cannot name.
* GCC takes the same inline-assembly path as clang, but no preset has built it below FULL.
* On macOS and iOS a module's offset is from its Mach-O header, not its vmaddr, so `sites.py` would have to add `__TEXT`'s base before asking `llvm-symbolizer`.
* On Linux, resolving ELF offsets as plain addresses is a hypothesis: right for a PIE, untested on a machine.
* Android runs on a device, so its sites would be resolved against the host's copy of the binary.
