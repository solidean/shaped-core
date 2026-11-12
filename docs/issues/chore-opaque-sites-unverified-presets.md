---
created: 2026-11-06
priority: someday
refs:
  - file: CMakePresets.json
  - file: tools/dev/lib/pipeline/sites.py
---

# The MSVC, GCC, iOS and Android development presets still build at SC_SOURCE_SITES=FULL

The clang development presets on Windows, Linux and macOS build at OPAQUE (docs/platforms.md, 'Source sites'); the others were left at FULL, unverified below it.

What moving each to OPAQUE needs checked, none of it measured yet:

* cl.exe captures a site with a call, not a `lea`, so a hot assert costs more; and its Debug link is incremental, so a test body's address may be a jump thunk the symbolizer cannot name.
* GCC takes the same inline-assembly path as clang, but no preset has built it below FULL.
* iOS shares macOS's module table and `atos` path, but its binaries run on a device, so its sites resolve against the host's copy.
* On Linux, resolving ELF offsets as plain addresses is a hypothesis: right for a PIE, untested on a machine.
* Android runs on a device, so its sites would be resolved against the host's copy of the binary.
