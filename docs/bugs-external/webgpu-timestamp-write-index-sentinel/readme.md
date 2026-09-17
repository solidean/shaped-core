# WebGPU: Dawn accepts 4294967295 as a timestamp write index, wgpu refuses it

**Status:** not filed; worked around in shaped-graphics' webgpu backend.
**Affects:** Emscripten's emdawnwebgpu port (the C API side), and Dawn's own leniency about the value.
Seen with the `webgpu` npm package 0.6.1 under node 22, and Deno 2.9.6's wgpu.
**Found by:** `shaped-graphics-test` failing under `--runtime deno` while passing under node, on the WebGPU timestamp-query test.

## What happens

A pass records timestamps through `GPUComputePassDescriptor.timestampWrites`, whose two indices are each **optional**:
leaving `endOfPassWriteIndex` out means "do not write a timestamp at the end of this pass".
The spec says a *provided* index must be less than the query set's count, so 4294967295 is out of range like any other too-large index.

The C API has no absent field, so it spells the same thing as a sentinel value, `WGPU_QUERY_SET_INDEX_UNDEFINED` = `UINT32_MAX` = 4294967295.
**emdawnwebgpu passes that sentinel straight to JavaScript** rather than mapping it back to an absent field.
Its `library_webgpu.js` reads both indices with a plain `u32` load and puts them in the descriptor as they are.

Dawn does not mind, because 4294967295 is its own sentinel.
wgpu does mind, and is right to:

```raw
In a pass parameter: Query 4294967295 is out of bounds for a query set of size 32
```

## Why it is not our bug

`repro.mjs` touches nothing of ours: it asks `navigator.gpu` for an adapter, makes a 32-query set, and records an empty compute pass three times.

| what the pass descriptor says | node 22 + `webgpu` 0.6.1 (Dawn) | Deno 2.9.6 (wgpu) |
|---|---|---|
| `endOfPassWriteIndex: 4294967295` | **accepted** | validation error, out of bounds |
| `endOfPassWriteIndex` omitted | accepted | accepted |
| `beginningOfPassWriteIndex: 32` of 32 | validation error, out of bounds | validation error, out of bounds |

The third row is the control: both implementations reject an ordinary out-of-range index, so the first row is about that one value rather than about the field or our call shape.
Omitting the field is accepted everywhere, which is what the C sentinel is supposed to mean.

So there are two separate things here, and neither is ours:
- **emdawnwebgpu should translate the sentinel** into an absent field, which is what makes the C API's "no write" reach the browser API.
- **Dawn accepts an out-of-range index** that the specification says it should refuse, which is what hid the first one from every Dawn-based runtime.

## Reproducing

```bash
uv run run.py                       # node and deno, whichever are on PATH
uv run run.py --node-modules DIR    # where the `webgpu` package lives, if not shaped-core's tools/dev/js
```

Exit 1 means some implementation still accepts the sentinel, exit 0 that none does, exit 2 that no runtime could be driven.
node needs the `webgpu` npm package; the script finds shaped-core's copy under `tools/dev/js/node_modules` by default.

## What we do about it

`sg::backend::webgpu` reserves the **last slot of every timestamp query set as a discard target**, and writes the end-of-pass timestamp there instead of passing the sentinel.
One slot per set is the whole cost — see `webgpu_query_system::usable_queries_per_set` and `webgpu_command_list::query_record_gpu_timestamp` in
[backends/webgpu/src/shaped-graphics/backends/webgpu/webgpu_query.cc](../../../libs/graphics/shaped-graphics/backends/webgpu/src/shaped-graphics/backends/webgpu/webgpu_query.cc).

## When to re-check

Run `run.py` after an emsdk bump, a `webgpu` package bump or a Deno bump.
The real fix is emdawnwebgpu mapping the sentinel to an absent field.
Once it does — or once `run.py` reports the bug gone — the backend can pass `WGPU_QUERY_SET_INDEX_UNDEFINED` again, and every slot of a query set becomes usable.
