# AMD: a DXIL module without `!llvm.ident` crashes the driver's compiler

**Status:** open, reproduced standalone, not yet filed upstream, worked around in SGL's DXIL writer; measured on 2026-11-07.
**Affects:** AMD D3D12 driver 32.0.21042.62 (`amdxc64.dll`), the Radeon iGPU of a Ryzen 9 9950X3D2 (RDNA 2), Windows 11; the system runtime and the Agility SDK alike.
**Found by:** `shaped-graphics-test` on that GPU, `SC_GPU=amd`: the native DXIL driver's first compute pipeline ended the process.

## What happens

`CreateComputePipelineState` on a DXIL module that has no `!llvm.ident` named metadata crashes with an access violation inside `amdxc64.dll`.
It never returns an error; the process ends.
LLVM leaves `!llvm.ident` optional, and DXC always writes one, so only a producer other than DXC meets it.
SGL's own DXIL writer was such a producer.

## The trigger

Each row is a fresh process creating one pipeline from one container.

| module | AMD | Intel Arc B580, 32.0.101.9150 |
|---|---|---|
| DXC's | compiled | compiled |
| DXC's, without `HASH` and `STAT` | compiled | — |
| DXC's, every record unabbreviated | compiled | — |
| DXC's, without `!llvm.ident` | **crashed** | compiled |
| SGL's | **crashed** | compiled |
| SGL's, with `!llvm.ident = !{!""}` | compiled | — |
| SGL's, with an `llvm.ident` naming SGL, or naming DXC | compiled | — |

So the bug needs exactly one thing: no `!llvm.ident` in the module.
Its string does not matter, an empty one included.

Before the identifier was found, SGL's module was brought closer to DXC's one difference at a time, and each change on its own left the crash in place:

- DXC's datalayout and shader flags, and DXC's struct names.
- Its metadata deduplicated, and its function-local constants moved into the function.
- Its unused types dropped, an empty `BLOCKINFO` added, and no metadata kinds beyond DXC's.

## Why it is not our bug

The variant `run.py` checks is DXC's own module with one named metadata node deleted, and nothing of ours touches it.
The deletion renumbers nothing, since named metadata takes no number, and DXC's disassembler reads the result back unchanged but for that node.
Intel's driver compiles the same container.
LLVM's bitcode format makes the node optional, and a missing optional node crashing a driver is the driver's to fix.

## Reproducing it

    uv run run.py [--dxc <dxc.exe>] [--vendor <hex PCI id>]

It builds `repro.cc` with clang-cl and compiles `shader.hlsl` with the newest Windows SDK's dxc unless `--dxc` names one.
Its variant deletes the module's `!llvm.ident` and gives the container the digest the runtime takes unsigned.
Each container becomes a pipeline in a process of its own, on the first AMD adapter unless `--vendor` names another (`8086` is Intel).
It exits 1 when the variant crashes and the control compiles, 0 when both compile, and 2 when the run is inconclusive.
On the machine above:

```
with !llvm.ident     compiled                 adapter: AMD Radeon(TM) Graphics
without !llvm.ident  crashed (exit 0xc0000005) adapter: AMD Radeon(TM) Graphics
REPRODUCED: the driver crashes compiling a module that has no !llvm.ident
```

## What we do about it

SGL's DXIL writer names its producer in every module, `!llvm.ident = !{!"sgl"}`.
[writer_stage.cc](../../../libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/dxil/impl/writer_stage.cc) does it in `name_producer`, which names this directory.
`slib - every module SGL's DXIL writes names its producer in llvm.ident, a library's too` holds it there.
That costs a module a few bytes and nothing else, so it stays when AMD fixes the driver.

## What to re-check

On a driver update, run `run.py` on an AMD GPU.
Exit 0 retires this entry; the identifier SGL writes stays either way.
