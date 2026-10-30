---
created: 2026-10-30
corpus:
  - bgfx/shaders/09-hdr/fs_hdr_blur
  - bgfx/shaders/09-hdr/fs_hdr_tonemap
---

# A `const` whose value is an expression over constants

`const a = 1.0 + 2.0` is `unsupported-yet: a const whose value is no literal, no enum case and no const` (CHK-219).
CHK-394 already folds operators over literals to a literal inside a body, so the spec has the folding and only the `const` declaration refuses it.

- **What needs it** is the `#define` arithmetic real shaders build tables from: bgfx's `blur9` normalizes its weights as `W0 + 2.0*(W1+W2+W3+W4)`.
- It blocks 2 of the first 104 bgfx translations in shader-rosetta, and will block more: weight tables and derived sizes are common.
- Minimal repro: the line above, and any entry point reading `a`.

`corpus:` names the shaders in shader-rosetta (`uv run dev.py companions init shader-rosetta`), under `sources/`.
