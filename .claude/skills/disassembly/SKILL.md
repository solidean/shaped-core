---
name: disassembly
description: Inspect the optimizer's actual codegen for shaped-core with `dev.py assembly` — search built symbols and disassemble a function (a local godbolt over the object files). Use when you need to confirm what machine code a function compiled to.
when_to_use: "disassemble", "assembly", "what does this compile to", "did the atomic fold", "did this inline", "is it vectorized", "look at the codegen", "dev.py assembly", "godbolt", "why is X slower/faster", "attribute a perf delta to a cause", "is this microbenchmark representative", "does the real code match the benchmark mock"
allowed-tools: Bash mcp__repo_tools__repo_search Read
---

## What this is

`dev.py assembly` reads the **object files** the current preset built and lets you
search symbols and disassemble a function — a local godbolt. Full reference:
[docs/guides/disassembly.md](../../../docs/guides/disassembly.md). Activate the
`building-and-testing` skill first if the preset isn't built yet.

**Before you attribute a perf delta to a cause, come here.** "It's slower because of
the TLS access / an extra load / a branch" is a *claim* — verify it in the codegen,
don't infer it from the numbers. And when a microbenchmark's mock disagrees with the
real code, pin the **real** symbol into the benchmark (a `CC_DONT_INLINE` probe) and
diff its disassembly against the mock's; the mock is usually the optimistic one
(it hoists / folds / DCEs what the real code can't).

```bash
uv run dev.py assembly search <pattern> [--preset P] [--target T] [--regex] [--all] [--limit N]
uv run dev.py assembly show   <symbol>  [--preset P] [--target T] [--source] [--att] [--bytes]
```

## The loop

1. **Build** the preset you want to inspect (`release-clang` for optimized code;
   `relwithdebinfo-clang` if you need `--source` interleave). `assembly` is
   read-only — it does not build for you.
2. **`search`** for the function. Pattern is a case-insensitive substring (or
   `--regex`) matched against mangled *and* demangled names; results group by
   target. Copy the symbol you want.
3. **`show`** it — exact mangled or demangled name (or a unique substring). Intel
   syntax, local branches labeled `.L0/.L1`, `call` targets folded from
   relocations, `lock`/atomics in red.

## Why objects, and the gotchas

- The linked release `.exe` is **stripped** (no symbol table, no PDB), so the tool
  scans `.obj` files — which keep full symbols in every config and hold a
  force-inlined function's real codegen.
- **Cross-object `call`/RIP targets are placeholders** in object code; `show`
  folds the relocation so the callee prints (mangled — `-C` can't combine with
  symbol selection).
- **`--source` needs a relwithdebinfo preset** (release objects carry no line
  info); it's best-effort.
- Addresses are section-relative (often `0`), so `show` selects by **name**.

## Pinpointing a hot loop

To land `show` on exactly one loop, extract it into a **uniquely-named,
`CC_DONT_INLINE`, anonymous-namespace function** and keep it referenced from a
test (an unreferenced TU-local noinline function is dead-code-eliminated). One
clean symbol, trivially searchable. Worked example:
`node_alloc_free_hotloop_probe` in
[allocation-benchmark.cc](../../../libs/base/clean-core/tests/benchmarks/allocation-benchmark.cc).

## Tips

- Colors honor the global `--plain` / `--colored`; pipe-safe by default.
- Missing tool? `assembly` finds `llvm-nm` / `llvm-objdump` beside the compiler or
  on `PATH`; override with `LLVM_NM` / `LLVM_OBJDUMP`.
