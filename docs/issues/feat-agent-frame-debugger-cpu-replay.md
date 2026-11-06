---
created: 2026-11-05
parent: epic-agent-frame-debugger
refs:
  - issue: feat-sgl-test-texture-bindings
---

# Replay one captured invocation of an SGL entry point on the interpreter, against the GPU's result

The interpreter runs any entry point exactly, with bindings a driver hands it; a capture provides those bindings and the invocation's stage inputs.
Its trace gives every print and every value along the way, so the replay steps through the `.sgl` source and names the first value where the GPU and the exact result part.
The interpreter samples textures and runs derivatives and subgroups, by the spec's rules (EVAL-120 to EVAL-145).
A replay needs them as the GPU computed them, from the capture, which those rules do not promise.
