---
created: 2026-11-12
priority: next
tags: [dev]
closed: 2026-11-12
resolution: fixed
---

# An object ninja records no header dependencies for is never rebuilt, and links a stale layout

On 2026-11-12 every shaped-graphics-test run here crashed in ~command_list, after 9f645e58c added a member to command_list.
ninja -t deps showed 'compute.cc.obj: #deps 0 (VALID)' for libs/graphics/shaped-graphics/src/shaped-graphics/command_list/compute.cc, so no header change rebuilt it.
It kept command_list's old layout, and the binary mixed two layouts: a free in ~command_list, a read of _rendering_targets, each depending on which members a test touched.
Two other objects had no dependencies either: clean-core's node-allocation-design-benchmark.cc and mbedtls's ecp_curves_new.c.
Deleting the three objects fixed it, and compute.cc.obj then recorded 442.
Not known how it lost them.
The build server is not a likely cause: two build directories lost exactly the same three entries, which points at something about those files.
`ninja -t deps | grep "#deps 0"` finds such an object in a second, and is the first thing to run on a binary that mixes two layouts.
dev.py could refuse, or rebuild, an object whose deps entry is empty, at build time or in doctor.

The same three objects had no dependencies in the `--agility-preview` build directory too, on 2026-11-12, with the same crash; deleting them fixed it there as well.
So it is these files rather than one bad compile: two build directories lost exactly the same three entries.

## Log

### 2026-11-12: closed in the build server

The two other objects are right to record none: each TU is #if'd out whole here, so it includes nothing.
compute.cc's empty record was not found again, so how it lost its headers stays unknown.
The optional build server no longer trusts a compile that reports no header at all: it reruns it every build, never stores or serves it,
and hands ninja a stale deps entry for it, so `--no-server` reruns it too.
Its own state file also drops a record line cut short mid-write, which could have loaded with fewer headers than its compile read.
