---
created: 2026-11-01
tags: [shaped-viewer]
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/capture.cc
---

# The first capture of a textured view in a process differs from every later one

A headless capture of a raster (untraced) view can be written before its textures' mip chains are generated, so the first capture of a scene's textures differs from every later one.

Seen from shelley (`libs/debug/shelley-render`, `sh::render::snapshot`), which captures through `sv::interactive` with `accumulate` 4 and an unlit material sampling a 3700 x 2100 base color texture:

- In one process, the same view captured four times in a row: the first differs from the other three, which are identical.
- The first shows the texture sharp and aliased at a grazing angle, the later ones filtered: the first sampled the full level, the mips not there yet.
- 6% of the pixels differ, by up to 173 steps; a traced capture of the same scene shows the same first-time difference, smaller.
- Across processes the result then depends on what was captured before (a texture already resident from an earlier capture comes with its chain), so a lit shot differs between runs.

`capture_session::is_settled` refuses while `pending_work > 0`, and mip generation is queued as pending work once `collect_settled` reports a landed texture.
So the window is somewhere between the acquire, the landing and the queuing.
Not narrowed down further.

shelley works around it with a small throwaway capture before each snapshot, which leaves the textures resident with their chains; every capture after it is identical, across runs too.

**Narrowed down (2026-11-01)**: not the settling, and not mips left ungenerated.
Each capture's viewer starts with fresh resources and waits: the pending mip work counts down from 11 to 0, the texture lands, and only then does the capture settle, for the first capture and later ones alike.
A declined mip pass stays pending (`record_pending_work`), and turning the BC7 swap off (`texture_policy::compress`) changes nothing.
So what differs is state that outlives a viewer in the process: the first capture samples the texture as if unfiltered, the later ones filtered.
Every view here, the unlit ones too, runs the traced path (`any_traced` is set), so the tracer's texture level-of-detail inputs are the next suspect.
