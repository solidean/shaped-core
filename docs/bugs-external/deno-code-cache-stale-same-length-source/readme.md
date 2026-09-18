# Deno: a rebuilt CommonJS script of the same length runs a stale code cache

**Status:** not filed; worked around in dev.py.
**Affects:** Deno 2.9.6 (V8 15.0), running a CommonJS script of 16384 bytes or more.
**Found by:** the threaded `graphics-rotating-cube-example` failing to start under `--runtime deno` after a rebuild of the same source.

## What happens

Deno keeps a V8 code cache per script path under `DENO_DIR`.
For a CommonJS script of 16 KiB or more, Deno hands that cache to V8 again after the file has changed, as long as the file's length has not.
V8 accepts it, because V8's own check on a code cache compares the source length rather than the content.

A cached lazy function carries its source offsets, and V8 parses it from those offsets on its first call.
In the rewritten file the offsets land mid-token, so the run dies with a SyntaxError naming a fragment of an identifier:

```raw
error: Uncaught (in promise) SyntaxError: Unexpected identifier 'one'
```

An Emscripten relink hits this easily.
Its `invoke_*` wrappers are emitted in an order that can change between builds of the same source, which keeps the file's length exactly.
The rotating-cube loader, 280 KB, failed that way twice, as `Unexpected identifier 'index'` and later `Unexpected identifier 'nction'`.
Both fragments come from the wrappers' own text, `function invoke_i(index) {`.
Swapping two wrappers by hand in a copy of the current loader reproduces it on demand, as `Unexpected identifier 'ii'`.

## Why it is not our bug

`repro.cjs` touches nothing of ours: two functions and one call, which `run.py` pads with a comment and rewrites in place with the two functions swapped.
Every case gets its own empty `DENO_DIR`, and each runs the script once to fill the cache, then the rewritten file.

| case (Deno 2.9.6) | second run |
|---|---|
| `.cjs`, 16384 bytes | **stale**, SyntaxError |
| `.js` with `--unstable-detect-cjs`, 16384 bytes — how dev.py ran it | **stale**, SyntaxError |
| `.cjs`, 16383 bytes | correct |
| `.cjs` with `--no-code-cache`, 16384 bytes | correct |
| `.cjs`, 16384 bytes, rewritten file at a new path | correct |
| `.mjs`, 16384 bytes | correct |

One byte of size separates the first row from the third, so the trigger is a size threshold in Deno's CommonJS path and not the script's content.
The new-path row shows the cache is keyed by path, and the `.mjs` row that ES modules check their cache properly.
32 KiB and 1 MiB variants go stale the same way as the first row.

## The evidence that it is the cache

Checked by hand on deno 2.9.6, with the two same-length variants `run.py` builds:

- **A, then B at the same path:** B fails with `SyntaxError: Unexpected identifier 'one'`.
- **B again with a fresh `DENO_DIR`, or after `deno clean`:** B runs correctly, so the failure lives in deno's cache directory and nowhere else.
- **`deno run -L debug`** logs the lookup: `V8 code cache hit for script: x.cjs, [6336263809937010070]`.
  A and B log **the same key**, though their contents differ, and B is then run from A's compiled code.
  So the key deno computes for a CommonJS script does not change with its content at equal length; the ES-module wrapper around it also hits under one unchanging key.
- **The 16 KiB threshold is empirical only:** 16383 bytes stays clean and 16384 goes stale, and nothing here explains why.

## The misdiagnosis this replaced

The failure first looked like `--unstable-detect-cjs` misreading the loader, because copying the loader to a `.cjs` name made it start.
It started because the copy was a new path with no cache entry.
A `.cjs` rebuilt in place goes stale exactly like the `.js`, which the first row shows.
So launching through a `.cjs` sibling would not have fixed anything, and the detection flag stays.

## Reproducing

```bash
uv run run.py                # the deno on PATH
uv run run.py --deno PATH    # a specific deno
```

Exit 1 means the bug reproduces, exit 0 that it is gone, exit 2 that deno could not be run.
A control row going stale is printed as a note, since it means the trigger has moved rather than gone.

## What we do about it

dev.py launches every wasm artifact under deno with `--no-code-cache`, in `JsRuntime.launch_prefix` in [tools/dev/lib/toolchain/jsruntime.py](../../../tools/dev/lib/toolchain/jsruntime.py).
Tests, `dev.py example`, `dev.py run`, benchmarks and the listing probe all take their argv prefix from there.
The cost is an uncached compile per launch, about 0.2 s for the rotating-cube loader.

## When to re-check

Run `run.py` after a Deno bump.
Once it reports the bug gone, drop `--no-code-cache` from `launch_prefix`.
