---
created: 2026-11-06
---

# shaped-graphics-test segfaults deno under wgpu

`uv run dev.py test shaped-graphics-test --preset emscripten-webgpu-relwithdebinfo --runtime deno` ends in about 5 s with exit 0xC0000005.
That is an access violation in the deno process itself, after printing only the run seed.
It reproduces on 556261fe9, so no recent sg change caused it, and `shaped-graphics-webgpu-test` passes under deno.
Nothing yet says which test reaches it, or whether it is wgpu's, deno's or a call of ours wgpu trusts.
No CI job runs this combination, so the sg suite has no wgpu coverage until this is found.
