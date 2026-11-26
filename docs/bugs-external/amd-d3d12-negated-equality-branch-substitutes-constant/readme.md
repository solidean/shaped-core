# AMD D3D12: a branch on a negated equality takes the compared constant as the value on the path where they differ

**Status:** reproduced standalone, not yet filed upstream; worked around in SGL's DXIL writer (EMIT-285).
**Affects:** AMD Radeon iGPU (RDNA 2), D3D12 driver 32.0.21042.62, Windows 11, on the system runtime and under Agility SDK 1.620 alike.
The Arc B580 beside it and WARP run the same container right.
**Found by:** `sg - a compute stage's failed assert reads the same through DXC and through the native route`, whose `assert v != 3` SGL lowers through `not`.

## What happens

Four threads read `v` from {1, 2, 3, 4}.
The one where `v` is 3 returns, and every other stores `v * 2 + 100 * index`, so the buffer must read {2, 104, 3, 308}.
`negated.dxil` writes the test as SGL did, a branch on `xor (icmp ne v, 3), true`, which is LLVM's `not`:

```
container      branch on                     AMD iGPU                 Arc B580          WARP
-------------  ----------------------------  -----------------------  ----------------  ----------------
negated.dxil   xor (icmp ne v, 3), true      wrong: {6, 106, 3, 306}  {2, 104, 3, 308}  {2, 104, 3, 308}
swapped.dxil   icmp ne v, 3, ways swapped    {2, 104, 3, 308}         {2, 104, 3, 308}  {2, 104, 3, 308}
```

The wrong row is `3 * 2 + 100 * index` in every surviving lane.
So the driver replaces `v` with 3 on the path where the condition says `v` is not 3: it propagates the equality into the wrong side of the branch.
`icmp eq` written directly is right, and so is a negated `v < 3`, so the trigger is the `xor` of an equality test.
Whether a `select` on the same `xor` goes wrong too has not been tried.

## Why it is not our bug

`repro.cc` is D3D12 and the C++ standard library, and the two containers differ in one instruction and the order of one branch's two targets.
Both are validated and signed, and the `xor` with `true` is the form LLVM itself writes for a `not` of an `i1`.
DXC folds it away at every optimization level, from `-Od` up, which is why HLSL never reaches it.
So a DXIL producer that keeps a `not` ahead of a branch meets it, and DXC never does.

## Reproducing it

    uv run run.py

It runs both containers on each hardware adapter named AMD, Intel or NVIDIA, and on WARP.
It exits 1 when the AMD adapter runs `negated.dxil` wrong and `swapped.dxil` right, 0 when it runs both right, and 2 when the run is inconclusive.
`shader.sgl` is what the containers were written from, and `negated.ll` and `swapped.ll` are their disassembly from `dxc -dumpbin`.

## What we do about it in the tree

EMIT-285: SGL's DXIL branches on what a `logical_not` negates, with the branch's two ways swapped, as DXC writes it.

## What to re-check after a driver update

Run `uv run run.py`; once it exits 0, the swap is no longer needed for AMD, though it stays DXC's shape and costs nothing.
