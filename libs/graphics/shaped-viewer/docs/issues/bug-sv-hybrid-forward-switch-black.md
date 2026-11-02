---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/rendering/hybrid_routine.cc
  - file: libs/graphics/shaped-viewer/examples/research-studio.cc
---

# A switch from hybrid deferred to forward once left the image black until another scene was loaded

Seen twice in research-studio on 2026-11-02, switching the renderer from hybrid deferred to hybrid forward, and not reproduced since.
The first time, on Country Kitchen with two hybrid bounces, the window froze and then closed.
The second time, on Salle de bain, the panel stayed live but the scene stayed black and the camera did not move.
Loading another scene and switching back brought the image back.

Measured: a fresh research-studio, switched after 30 deferred frames, recovers on both scenes at one and two bounces.
It draws no samples for 170 to 270 frames, about half a second, while forward's pipelines build, and is then correct.
Pacing those frames at 16 ms each, as a vsynced window does, changes nothing, so the wait is in time and not in frames.

Hypothesis: both sessions had been open across a change to the instance record's layout (224 to 256 bytes).
Forward compiles its material permutations from the `.sgl` sources on disk at the switch, so a running binary met shaders reading a record it does not write.
If so, the bug is that a reloaded shader is accepted against a binary whose generated layouts it no longer matches, rather than anything in the switch.
If it recurs on a session started after the last shader change, that hypothesis is wrong.
