# Disassembly (`dev.py assembly`)

A local [godbolt](https://godbolt.org) over the object code the current preset
produced: search for a symbol, then read its disassembly — useful for confirming
what the optimizer actually emitted (does the atomic fold away? did this inline?
is the loop vectorized?). Back to [guides](_index.md).

This is the static view — the code the CPU *might* run.
For the runtime cost it actually paid — instructions retired, cache misses, branch mispredicts — see [profiling.md](profiling.md) (nexus/bench hardware counters).

```bash
uv run dev.py assembly search <pattern>    # find symbols (mangled + demangled), grouped by target
uv run dev.py assembly show <symbol>       # disassemble one function
uv run dev.py assembly trace --target T --symbol S -- <args>   # what one invocation actually ran
```

`search`/`show` answer the **static** question — what the code might do.
[`trace`](#trace) answers the **dynamic** one: which branch a real invocation took, where an indirect call landed, how many instructions it retired, and which memory it actually touched.

None of this is specific to shaped-core: `--build-dir` / `--objects` point `search`/`show` at any build tree, and `trace --exe` traces any executable — see [Other projects](#other-projects).

## Why object files (and why that's fine)

The tool reads **`.obj` files**, not the linked `.exe`/`.dll`.
On Windows the release executable is *stripped of its COFF symbol table* (2 symbols total) and release builds emit no PDB, so there is nothing to search or name there.
Every object file, by contrast, keeps a full mangled symbol table in every configuration.

For reading optimized codegen this is an advantage rather than a compromise.
A `CC_FORCE_INLINE`-heavy function is already fully inlined *within* its object at compile time, so the `.obj` holds its real hot-path instructions.
The one thing object code lacks is applied relocations, so **cross-object `call`/RIP targets are placeholders**.
`show` runs `objdump -r` and folds the relocation in, so calls still print their real callee, mangled.
Local control flow — the loops and branches you care about — resolves fully.

Symbols are grouped by the CMake target that owns them, recovered from the `.../CMakeFiles/<target>.dir/...` path.
`--target` (comma-list, wildcards, repeatable) restricts the scan; the default is every target under the preset.

## search

```bash
uv run dev.py assembly search "node_allocation_free" --preset release-clang
uv run dev.py assembly search "cc::vector" --target clean-core --limit 40
uv run dev.py assembly search "operator new" --all           # include data symbols too
uv run dev.py assembly search "alloc_.*fast" --regex
```

Pattern is a case-insensitive substring, or a full regex with `--regex`, matched against **both** the mangled and the demangled name.
By default only text (function) symbols are listed — the disassemblable ones; `--all` adds data.
Output is grouped per target with the artifact size and total symbol count, each match printed as its mangled name plus the demangled form in parentheses.
A final line reports how much was searched, and `--limit` caps the rows (default 100).

## show

```bash
uv run dev.py assembly show "cc::node_allocation_free_large" --preset release-clang
uv run dev.py assembly show "?allocate_node_bytes_non_fast@node_allocator@cc@@..."  # exact mangled
```

Takes an exact mangled **or** demangled name.
With no exact match, a string that uniquely identifies one symbol selects it, and an ambiguous one lists the candidates.
Output is Intel syntax with:

- **local branches labeled** (`.L0`, `.L1`, …) — loops read like source;
- **relocations folded in** so `call` shows its real callee;
- **light color** (dimmed addresses, bold mnemonics, `lock`/atomics in red, jumps
  and `call`/`ret` in yellow) when the terminal supports it — honors the global
  `--plain` / `--colored`.

Flags: `--att` (AT&T syntax; skips the label/color pass), `--bytes` (show raw instruction bytes), `--source` (interleave source — best-effort, see below).

## Reading a specific hot loop: the named-probe pattern

The reliable way to point `show` at exactly the code you care about is to extract it into a **uniquely-named, non-inlined, TU-local function**.
A template lambda in a benchmark compiles to a mangled name that is hard to find and may be inlined away; a `CC_DONT_INLINE` free function in an anonymous namespace compiles to one clean symbol.
Keep it alive with a reference from a test — an unreferenced TU-local `noinline` function is dead-code-eliminated.
See `node_alloc_free_hotloop_probe` in [allocation-benchmark.cc](../../libs/base/clean-core/tests/benchmarks/allocation-benchmark.cc) for a worked example.
It isolates the node allocator's fast path, so `assembly show node_alloc_free_hotloop_probe` lands on the `lock and` (allocate) and `lock or` (free) directly.

## trace

`show` tells you what the optimizer emitted; `trace` tells you what the CPU actually ran.
It launches a target under the Win32 debug API, breaks on a symbol, skips the warm-up hits, and single-steps one invocation:

```bash
uv run dev.py assembly trace --target clean-core-test \
    --symbol "cc::async_node_base::schedule" --skip 100 -- "async - basic"
```

Everything after `--` goes to the traced binary — above, a nexus test-name filter, so only the test that exercises the code runs.
`--skip N` walks past N entry hits to reach a steady-state call rather than the cold first one.

Because each annotation comes from where the CPU actually went next, `trace` is exact where `show` has to guess:

```
je   0x1120342a        ; taken -> mymodule.exe!zero_path
call rax               ; -> allocator.dll!allocate+0x20
```

Reach for it when the question is *which* path ran, not which paths exist: real branch outcomes, virtual/indirect dispatch targets, and the true instruction count of an invocation.
**Windows x64 only**, and it needs a `relwithdebinfo-*` preset — release has no PDB, so the trace degrades to raw addresses.

`dev.py assembly trace` mirrors the tracer's own flags, except that `--target` here names a *build target* of this repo and the tracer's `--target` spec is spelled `--spec`.
A flag you omit is not passed on at all, so the defaults documented in the tracer's [readme](../../tools/instruction-tracer/readme.md) are the ones you get.

### What `trace` can print

`--sections <list>` composes the output, all of it rendered from **one** capture; the default is `trace` alone.
The names are `trace`, `stats`, `memory`, `cachelines`, `memory-stats` and `timing`, and `--stats` is the shortcut for `--sections stats`.

- **`stats`** answers "where did the instructions go" without reading an 800-line trace: one row per symbol, sorted by cost.
  The columns are self instructions, atomics, direct/indirect calls, memory reads/writes and branches taken.
  Usually the first move on a new probe; read the trace afterwards for the rows that look wrong.
- **`memory` / `cachelines` / `memory-stats`** resolve each memory operand's effective address at run time and show what the run actually touched.
  This is as close as the tracer gets to the cache miss `stats` cannot see.
  It names *which* data you touch and how densely, so a scattered-access or half-used-line pattern shows up even though the miss latency does not.
- **`timing`** feeds the retired stream to `llvm-mca` for a static cost model — the number behind `stats`' `slow` flag, and `dev.py` resolves `llvm-mca` automatically.
- **`--html <path>`** writes the whole capture, plus a godbolt-style source view, to one self-contained page to open in a browser.

```bash
uv run dev.py assembly trace --target clean-core-test --symbol single_lazy_probe --skip 2 \
    --sections stats,cachelines,memory-stats -- "bench-async (single-thread drive)"
```

What each section means, which regions the memory views show, and every caveat on the timing model are the tracer's [readme](../../tools/instruction-tracer/readme.md#output).
The one worth carrying here: any non-trace section raises the `--instructions` default to `100000`.
Single-stepping costs a debug-event round trip per instruction, so a full capture is much slower than a plain trace.

## Other projects

Nothing about the machinery is shaped-core-specific — objects are objects, and the tracer takes a
path to an `.exe`.
Three flags open all of it up to any other project, whatever built it:

```bash
# any build tree, grouped by target
uv run dev.py assembly search "render::" --build-dir D:/proj/out/build/msvc-release
uv run dev.py assembly search "operator new" --objects D:/proj/obj,D:/other/obj

# a single object file
uv run dev.py assembly show "foo::bar" --objects D:/proj/obj/mylib.dir/foo.obj

# any executable, with its own arguments
uv run dev.py assembly trace --exe D:/proj/out/bin/app.exe --symbol "render::draw" \
    --skip 50 --sections stats,cachelines -- --scene foo.json
```

`--build-dir` (one tree) and `--objects` (a directory or a single `.obj`/`.o`, comma-list and
repeatable) both switch `search`/`show` into **external mode**: no `CMakePresets.json` is read, no
CMake discovery runs, nothing is configured or built.
They therefore don't combine with `--preset`, which would silently do nothing — that's an error.

**Target grouping degrades gracefully.** A CMake tree still names targets exactly, from the
`.../CMakeFiles/<target>.dir/...` path segment.
Anything else — MSBuild, cargo, a hand-rolled makefile — groups by the object's directory relative
to the scan root, so an MSBuild tree lists `WPFDXInterop/samples/D3D11Image/x64/Debug` rather than
one undifferentiated bucket, and `--target "WPFDXInterop/*"` still filters it.

**`trace --exe` skips the build**, because there is nothing here that could build it, and
`--target` / `--exe` are mutually exclusive.
`instruction-tracer` itself is still built and located from *this* repo's preset — it is our tool,
and the preset only decides which build of it runs.
The debuggee's working directory defaults to the exe's own directory (an external app resolves its
DLLs and data relative to itself); `--cwd PATH` overrides it.
Instead of guessing from a preset name, the PDB check is direct: `trace` warns when no `.pdb` sits
beside the exe, since without one the trace degrades to raw addresses.

**Relative paths in `--build-dir`, `--objects`, `--exe`, `--cwd` and `--html` resolve against your
current directory**, not the shaped-core root — so you can `cd` into the other project and type
short paths.
That is the *only* thing the working directory decides: `dev.py` never infers which project it is
looking at from where you stand, and never changes directory itself.

**Finding LLVM.** `llvm-nm` / `llvm-objdump` are looked up in the env override, then `PATH`, then
beside the compiler recorded in the scanned tree's `CMakeCache.txt`, and finally beside the one in
this repo's default preset.
That last fallback is what makes an MSVC-built or non-CMake tree work on Windows, where LLVM's
`bin/` is usually off `PATH`.
`LLVM_NM` / `LLVM_OBJDUMP` / `LLVM_MCA` override all of it.

## Limitations

These apply to `search`/`show`; `trace` has its own list in its [readme](../../tools/instruction-tracer/readme.md#limits).

- **Release only shows the code, not source.** `--source` needs debug line info,
  which release objects don't carry — run a **relwithdebinfo** preset for source
  interleave (`--preset relwithdebinfo-clang`). It stays best-effort.
- **Cross-object call targets stay mangled.** `-r` names the callee, but `-C` cannot be combined with `--disassemble-symbols` — it would make the symbol lookup miss.
  So the folded reloc target is not demangled.
  The function name still leads the mangled string, so it stays readable.
- **Object addresses are section-relative** (often `0` for COMDAT), so `show`
  works by symbol name, not by absolute address.

## Under the hood

`assembly` shells out to LLVM's `llvm-nm` (symbol enumeration, including MSVC-ABI
demangling via `--demangle`) and `llvm-objdump` (`-d -r`), found beside the
configured compiler (`C:\Program Files\LLVM\bin` on this setup) or on `PATH`;
override with `LLVM_NM` / `LLVM_OBJDUMP`. The reusable logic lives in
[tools/dev/lib/toolchain/disasm.py](../../tools/dev/lib/toolchain/disasm.py), the
command in [tools/dev/cmd/assembly.py](../../tools/dev/cmd/assembly.py).
