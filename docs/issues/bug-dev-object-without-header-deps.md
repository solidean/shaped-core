---
created: 2026-11-12
priority: next
tags: [dev]
---

# An object ninja records no header dependencies for is never rebuilt, and links a stale layout

On 2026-11-12 every shaped-graphics-test run here crashed in ~command_list, after 9f645e58c added a member to command_list.
ninja -t deps showed 'compute.cc.obj: #deps 0 (VALID)' for libs/graphics/shaped-graphics/src/shaped-graphics/command_list/compute.cc, so no header change rebuilt it.
It kept command_list's old layout, and the binary mixed two layouts: a free in ~command_list, a read of _rendering_targets, each depending on which members a test touched.
Two other objects had no dependencies either: clean-core's node-allocation-design-benchmark.cc and mbedtls's ecp_curves_new.c.
Deleting the three objects fixed it, and compute.cc.obj then recorded 442.
Not known how it lost them: likely a compile through the build server that handed back no include list, since touching the header and building with --no-server did not rebuild it either.
dev.py could refuse, or rebuild, an object whose deps entry is empty, at build time or in doctor.
