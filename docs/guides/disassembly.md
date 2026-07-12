# Disassembly (`dev.py assembly`)

A local [godbolt](https://godbolt.org) over the object code the current preset
produced: search for a symbol, then read its disassembly — useful for confirming
what the optimizer actually emitted (does the atomic fold away? did this inline?
is the loop vectorized?). Back to [guides](_index.md).

```bash
uv run dev.py assembly search <pattern>    # find symbols (mangled + demangled), grouped by target
uv run dev.py assembly show <symbol>       # disassemble one function
```

## Why object files (and why that's fine)

The tool reads **`.obj` files**, not the linked `.exe`/`.dll`. On Windows the
release executable is *stripped of its COFF symbol table* (2 symbols total) and
release builds emit no PDB, so there is nothing to search or name there. Every
object file, by contrast, keeps a full mangled symbol table in every
configuration.

For reading optimized codegen this is an advantage, not a compromise: a
`CC_FORCE_INLINE`-heavy function is already fully inlined *within* its object at
compile time, so the `.obj` holds its real hot-path instructions. The one thing
object code lacks is applied relocations, so **cross-object `call`/RIP targets are
placeholders** — `show` runs `objdump -r` and folds the relocation in, so calls
still print their real callee (mangled). Local control flow — the loops and
branches you care about — resolves fully.

Symbols are grouped by the CMake target that owns them, recovered from the
`.../CMakeFiles/<target>.dir/...` path. `--target` (comma-list, wildcards,
repeatable) restricts the scan; the default is every target under the preset.

## search

```bash
uv run dev.py assembly search "node_allocation_free" --preset release-clang
uv run dev.py assembly search "cc::vector" --target clean-core --limit 40
uv run dev.py assembly search "operator new" --all           # include data symbols too
uv run dev.py assembly search "alloc_.*fast" --regex
```

Pattern is a case-insensitive substring (or a full regex with `--regex`), matched
against **both** the mangled and the demangled name. By default only text
(function) symbols are listed — the disassemblable ones; `--all` adds data.
Output is grouped per target with the artifact size and total symbol count, each
match printed as its mangled name plus the demangled form in parentheses, and a
final line reporting how much was searched. `--limit` caps the rows (default 100).

## show

```bash
uv run dev.py assembly show "cc::node_allocation_free_large" --preset release-clang
uv run dev.py assembly show "?allocate_node_bytes_non_fast@node_allocator@cc@@..."  # exact mangled
```

Takes an exact mangled **or** demangled name; if there's no exact match but the
string uniquely identifies one symbol it uses that, and if it's ambiguous it lists
the candidates. Output is Intel syntax with:

- **local branches labeled** (`.L0`, `.L1`, …) — loops read like source;
- **relocations folded in** so `call` shows its real callee;
- **light color** (dimmed addresses, bold mnemonics, `lock`/atomics in red, jumps
  and `call`/`ret` in yellow) when the terminal supports it — honors the global
  `--plain` / `--colored`.

Flags: `--att` (AT&T syntax; skips the label/color pass), `--bytes` (show raw
instruction bytes), `--source` (interleave source — best-effort, see below).

## Reading a specific hot loop: the named-probe pattern

The reliable way to point `show` at exactly the code you care about is to extract
it into a **uniquely-named, non-inlined, TU-local function**. A template lambda in
a benchmark compiles to a mangled name that's hard to find and may be inlined
away; a `CC_DONT_INLINE` free function in an anonymous namespace compiles to one
clean symbol. Keep it alive with a reference from a test (an unreferenced
TU-local `noinline` function is dead-code-eliminated). See
`node_alloc_free_hotloop_probe` in
[allocation-benchmark.cc](../../libs/base/clean-core/tests/benchmarks/allocation-benchmark.cc)
for a worked example — it isolates the node allocator's fast path so
`assembly show node_alloc_free_hotloop_probe` lands on the `lock and` (allocate)
and `lock or` (free) directly.

## Limitations

- **Release only shows the code, not source.** `--source` needs debug line info,
  which release objects don't carry — run a **relwithdebinfo** preset for source
  interleave (`--preset relwithdebinfo-clang`). It stays best-effort.
- **Cross-object call targets stay mangled.** `-r` names the callee, but `-C`
  can't be combined with `--disassemble-symbols` (it would make the symbol lookup
  miss), so the folded reloc target isn't demangled. The function name still leads
  the mangled string, so it's readable.
- **Object addresses are section-relative** (often `0` for COMDAT), so `show`
  works by symbol name, not by absolute address.

## Under the hood

`assembly` shells out to LLVM's `llvm-nm` (symbol enumeration, including MSVC-ABI
demangling via `--demangle`) and `llvm-objdump` (`-d -r`), found beside the
configured compiler (`C:\Program Files\LLVM\bin` on this setup) or on `PATH`;
override with `LLVM_NM` / `LLVM_OBJDUMP`. The reusable logic lives in
[tools/dev/lib/toolchain/disasm.py](../../tools/dev/lib/toolchain/disasm.py), the
command in [tools/dev/cmd/assembly.py](../../tools/dev/cmd/assembly.py).
