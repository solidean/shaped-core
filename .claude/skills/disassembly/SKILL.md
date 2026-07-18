---
name: disassembly
description: Inspect the optimizer's actual codegen for shaped-core with `dev.py assembly` — search built symbols, disassemble a function (a local godbolt over the object files), and trace what a function really executed at run time. Use when you need to confirm what machine code a function compiled to, or which path it actually took.
when_to_use: "disassemble", "assembly", "what does this compile to", "did the atomic fold", "did this inline", "is it vectorized", "look at the codegen", "dev.py assembly", "godbolt", "why is X slower/faster", "attribute a perf delta to a cause", "is this microbenchmark representative", "does the real code match the benchmark mock", "which branch was taken", "where did that indirect call go", "what did it actually execute", "trace instructions", "how many instructions"
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
uv run dev.py assembly trace  --target T (--symbol S | --address A | --spec X) [--skip N] [--traces N] [--sections L] [--memory-regions L] -- <args>
```

**`search`/`show` are static — `trace` is dynamic.** When the question is "what *did* it do" rather
than "what *might* it do" — which branch was taken, where an indirect/virtual call landed, how many
instructions an invocation actually retired — use `trace`. It runs the binary under a debugger,
breaks on the symbol, and single-steps one invocation
([tools/instruction-tracer](../../../tools/instruction-tracer/readme.md); Windows x64, needs a
`relwithdebinfo-*` preset for symbols). `--skip N` walks past warm-up hits to reach a steady-state
call — see the warm-up rule below, it is not optional.

**`--stats` first, then read the trace.** It replaces the trace with one row per symbol — self
instructions, atomics, slow ops, direct/indirect calls, memory reads/writes, branches taken — sorted
by cost. An 800-line trace is a bad first look; the table tells you which rows are worth reading, and
the atomics column earns its keep (10 of 797 instructions were ~43% of the cycles on the async probe).
Do not hand-bucket a trace with a throwaway script — that is what this flag is.

The `slow` column catches what an instruction count cannot: `idiv` (a `%` on a non-power-of-two),
`rdtsc`, fences, `pause` (a spinlock actually spinning), `rep` string ops. A footer names them — it
is how we found that `std::unordered_map` float-divides on every insert for its load factor. All
zeros is also a result: it means the count is a fair proxy. It cannot see a cache miss, which is
usually what actually costs you.

**Where the data went, not just the code.** `--sections` composes any of `trace,stats,memory,`
`cachelines,memory-stats` into one capture (`--stats` is the shortcut for `stats`). The memory
sections resolve each memory operand's *runtime* address and show what the invocation actually
touched: a raw chronological list, a **cacheline** view (per-line access count + how many of the 64
bytes were touched — the "am I using the whole line or 8 bytes of it" check), and a per-symbol
memory table. Every address is tagged by region — `heap` (allocations + named globals), `stack`
(another function's frame, reached through a passed-in span), `frame` (the current function's own,
off by default), `instructions` (code fetches / I-cache footprint, off by default); `--memory-regions`
picks the set. This is the closest the tracer gets to the cache miss `--stats` is blind to: it shows
*which* data you touch and how densely, so a scattered-access or half-used-cacheline pattern is
visible even though the miss latency itself is not.

```bash
uv run dev.py assembly trace --target clean-core-test --symbol single_lazy_probe --skip 2 \
    --sections stats,cachelines -- "<test name>"
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
clean symbol, trivially searchable. Worked examples:
`node_alloc_free_hotloop_probe` in
[allocation-benchmark.cc](../../../libs/base/clean-core/tests/benchmarks/allocation-benchmark.cc),
`single_lazy_probe` in
[async-benchmark.cc](../../../libs/base/clean-core/tests/benchmarks/async/async-benchmark.cc).

### Warm the probe, then `--skip` past the cold hits

**A probe's first invocation is not the steady state, and `trace` breaks on the
first hit by default.** One-time costs hide there — a container's first growth (a
real `malloc`), a lazy init, a cold branch predictor, an unresolved icall. Trace
that and you will confidently describe a path the benchmark never actually pays.

So: call the probe **several times on the same state** the benchmark reuses, and
trace with `--skip N` to land on a settled call. Say which N in the probe's
comment, so the next reader doesn't re-learn it.

```cpp
// Called repeatedly on ONE scheduler: the first enqueue grows the queue vector
// from zero capacity (a real mi_malloc_aligned) that the steady state never pays.
for (i64 i = 0; i < 3; ++i)
    bench::sink ^= single_lazy_probe(sched, 7 + i);
```
```bash
uv run dev.py assembly trace --target clean-core-test --symbol single_lazy_probe --skip 2 -- "<test name>"
```

Sanity check: if a trace shows an allocator, a lock, or an init-guard you did not
expect on a hot path, suspect a cold hit before you believe the finding.

## Tips

- Colors honor the global `--plain` / `--colored`; pipe-safe by default.
- Missing tool? `assembly` finds `llvm-nm` / `llvm-objdump` beside the compiler or
  on `PATH`; override with `LLVM_NM` / `LLVM_OBJDUMP`.
