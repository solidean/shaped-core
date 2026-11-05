---
created: 2026-11-05
parent: epic-agent-frame-debugger
---

# Capture a frame of sg: its command lists, pipelines, bindings and the resources they read and wrote

The capture is a value, as a recording is in clean-core: what each submit recorded, the pipeline and bindings of every draw and dispatch, and the bytes of the resources it reached.
Its size decides what is kept whole and what is kept by hash or on request; a pixel query needs the targets and the bindings of the draws that touched it, not every texture of the frame.
