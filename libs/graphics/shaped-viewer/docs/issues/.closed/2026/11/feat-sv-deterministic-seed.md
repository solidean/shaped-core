---
created: 2026-10-19
refs:
  - symbol: sv::seed_of
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/rendering/view_renderer.cc
  - file: libs/graphics/shaped-viewer/docs/wavefront.md
closed: 2026-11-11
resolution: fixed
---

# A seed that makes a capture the same image on any machine

A traced layer's seed hashes its slot's restart count with its frame, and how many restarts come before a capture's final run depends on timing: cold compiles and owed-work drains each add one.
So two runs of one capture hold the same sample count but not always the same image.

The seed needs a hash of machine-independent inputs instead.
`trace_hash` cannot serve as one: it folds in `inst.blas.get()` and the records' bindless indices, which differ between machines and between runs.
Each pointer and slot index has to be replaced by its content identity before it is hashed.

Done when a test captures one scene in two processes, with the restarts between them made to differ, and the two images are equal bit for bit.

## Resolved

The seed is `seed_key_of` mixed with the frame within the run, and no restart count is kept any more.
The key hashes the camera, the resolution, the background, the light records, and per instance its transform, its geometry's content key and its shader key.
A material's parameters and the decals are left out, since their keys name ids a process mints; an edit to them alone keeps the sequence, while the mean still restarts.
The hybrid's jitter index counted restarts too, and now starts from the key.

`tests/deterministic-seed-test.cc` renders the lit Cornell box twice in one process, a manager and a store each.
The second run first draws another mesh in another view and takes a detour with the camera, so its ids, acceleration structures and restarts differ.
On an M4 the means are equal bit for bit in the megakernel, the hybrid and the wavefront, and two probes fail it: a BLAS address in the key, and a restart count in it.
It is two runs in one process rather than two processes; the bindless indices came out equal in both runs, since a frame mints them in draw order.
It drives the single-view renderer; the plan path sets the key the same way, and no test captures through it.
