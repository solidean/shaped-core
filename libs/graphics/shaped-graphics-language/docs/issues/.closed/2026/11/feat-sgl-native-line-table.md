---
created: 2026-11-06
parent: epic-sgl-native-ir-targets
closed: 2026-11-07
resolution: fixed
---

# SGL lines in the SPIR-V the native layer writes

Each operation keeps its flat statement, and so its line, but the writer emits no OpLine or OpString yet, so a validation message and RenderDoc name no .sgl line.

## Landed on 2026-11-07

EMIT-257 states it, on both native writers, as an opt-in: `text_request::native_line_table`, slib's `native_line_table` option, and `vulkan_config` and `dx12_config::native_line_tables`.
The line table is one target-neutral table, `ir::line_table`, which emit builds from `check::source_lines` and each writer reads.

- **SPIR-V:** `OpString`, an `OpSource` holding the file's text, and an `OpLine` where the line changes.
  `NonSemantic.Shader.DebugInfo.100` was left out: core lines name the file and line, which is what was asked, and no tool here could show that they fall short.
- **DXIL:** DXC's `-Zi` form, measured on DXC 1.9.2602.
  DXC's program part is byte for byte the same with and without `-Zi`, and the debug info is a second module in an `ILDB` part with an `ILDN` name.
  SGL writes the same: `DILocation`s with their `inlinedAt` chain, a `DISubprogram` for the entry point and one per file it inlines from, and `dx.source.contents`.
  Embedded rather than a separate PDB, since a container then carries all a debugger needs and nothing has to find a file.

## Measured on 2026-11-07

On the Arc B580's machine, with Agility:

- **Off writes the same bytes.** 684 native compiles of the repo's SGL hash the same before and after the change.
- **On changes no instruction.** The same 684, compared with the option on: every SPIR-V instruction but the debug ones, and every `DXIL` part byte for byte.
- **Both validators take it.** `spirv-val` passes every lowered module with its table, and DXC's validator 293 containers, the viewer's tracer and slug-traced among them.
- **DXC reads it back.** Its disassembly of an `ILDB` names each `.sgl` line, an inlined statement in its own scope at its call, and the source text.
- **A test can fail.** Shifting every line by one failed the tests of both writers.
- **The viewer's `path_trace`, `release-clang`, best of 40:**

| | off | on |
|---|---|---|
| DXIL | 5.79 ms, 29,236 bytes | 6.68 ms, 187,652 bytes |
| SPIR-V | 5.01 ms, 63,248 bytes | 5.19 ms, 151,208 bytes |

The size is the text of each file a line names, which for the tracer is its modules, and DXIL's second copy of the module.

## Not confirmed

- **No debugger read it.** Neither RenderDoc nor PIX was driven here, so the table is verified by its format and by DXC's own tools.
- **Columns are 0.** The table holds a statement's line alone.
- **Two calls on one line are one site.** An inlined chain names files and lines, not call sites, so DXIL repeats an `inlinedAt` per statement rather than sharing one per call.
