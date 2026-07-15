# instruction-tracer

Records what optimized x64 code **actually executed** — the retired instructions of a real
invocation, with the branches it really took and the indirect calls it really made.

`dev.py assembly` (the [disassembly](../../.claude/skills/disassembly/SKILL.md) skill) answers the
static question: what *might* this code do. This answers the dynamic one. It launches a program
under the Win32 debug API, breakpoints a symbol, skips the warm-up hits, then single-steps one
invocation and prints it.

It is deliberately not a general debugger. The narrow scope is what makes a few hundred lines of
debug loop preferable to DynamoRIO or Pin.

**Windows x64 only.** Built by default in a top-level build (`SC_BUILD_TOOLS`), skipped when
shaped-core is consumed via `add_subdirectory`.

## Quick start

Drive it through `dev.py`, which builds both binaries and resolves their paths — never construct
build paths by hand:

```bash
uv run dev.py assembly trace --target clean-core-test \
    --symbol "cc::async_node_base::schedule" \
    -- "async - basic"
```

Everything after `--` goes to the debuggee verbatim — above, a nexus test-name filter, so the
process runs just the test that exercises the code you care about.

`assembly trace` is the third member of the [disassembly](../../docs/guides/disassembly.md)
workflow: `search` finds a symbol, `show` disassembles it statically, `trace` shows what it really
did.

## Usage

`dev.py assembly trace` mirrors the flags below, with two differences: `--target` there names the
*build target* to trace (this tool's `--exe`), and the tracer's `--target` spec is spelled `--spec`.
Flags you omit are not passed at all, so the defaults documented here are the only ones.

```
uv run dev.py assembly trace --target <build-target>
                             (--symbol <name> | --address <hex> | --spec <spec>)
                             [options] [-- <args passed to the debuggee>]
```

The tool's own CLI, for reference (or when running it directly):

```
instruction-tracer --exe <path> (--symbol <name> | --address <hex> | --target <spec>)
                   [options] [-- <args passed to the debuggee>]
```

### Target — exactly one

| Flag | Example | Notes |
|---|---|---|
| `--symbol <name>` | `--symbol "foo::bar"` | Exact match first, then a unique substring. Always a symbol, even if it looks like an address. |
| `--address <hex>` | `--address 0x7ff611203410` | Absolute runtime address. Accepts windbg's `` 7ff6`11203410 `` grouping. |
| `--target <spec>` | see below | Form inferred from the separators. |

`--target` accepts `foo::bar`, `0x7ff6...`, `mod.exe!foo::bar`, and `mod.exe+0x3410`. Module names
are case-insensitive and a bare stem works (`mymodule` finds `mymodule.exe`). `!` binds tighter than
`+`, so `mod.dll!operator+` parses as you would want.

Prefer `mod.exe+0x3410` over `--address` when scripting: it is immune to ASLR, which re-bases each
image once per boot.

An ambiguous substring fails with every candidate listed, rather than picking one:

```
symbol 'itrace_fixture_' is ambiguous:

  00007ff6`3f4c1020  instruction-tracer-fixture.exe!itrace_fixture_mul
  00007ff6`3f4c1000  instruction-tracer-fixture.exe!itrace_fixture_add

narrow the spec, or use --target module!symbol / --address.
```

### Options

| Flag | Default | Meaning |
|---|---|---|
| `--skip <n>` | `0` | Ignore the first n entry hits. The first recorded trace is hit n+1. |
| `--traces <n>` | `1` | Record n invocations, counted across all threads. |
| `--instructions <n>` | `100` | Max retired instructions per trace. |
| `--until-return` | on | Stop once the entry frame returns. |
| `--stop-at-syscall` | on | Stop before executing a syscall, rather than stepping into the kernel. |
| `--stack` | on | Print the stack at entry. |
| `--source` | on | Annotate with source file/line and the source text. |
| `--register-diffs` | off | Show the registers each instruction changed. |
| `--terminate-after-traces` | on | Kill the debuggee once the last trace lands. |

Every boolean has a `--no-` form (`--no-source`). `-h` / `--help` prints all of this.

Output is colored when stdout and stderr are both terminals; `--colored` / `--plain` override, and
`NO_COLOR` / `FORCE_COLOR` are honored (`NO_COLOR` wins). Same policy as `dev.py`, and driving it
via `dev.py assembly trace` makes dev.py's choice authoritative.

Exit codes: `0` traced something, `1` bad usage or launch failure, `2` the target never resolved
(unknown, ambiguous, or never entered).

## Output

```
=== trace 1/1: instruction-tracer-fixture.exe!itrace_fixture_add ===
thread: 41340
hit:    101
entry:  instruction-tracer-fixture.exe!itrace_fixture_add
return: instruction-tracer-fixture.exe!drive+0x1c

stack:
  itrace_fixture_add                      ...\fixture\main.cc:14
  drive                                   ...\fixture\main.cc:33
  main                                    ...\fixture\main.cc:42

...\fixture\main.cc:17
    return x + y;

  00007ff6`3f4c1008  mov ecx, [rsp+0x04]
  00007ff6`3f4c100c  mov eax, [rsp]
  00007ff6`3f4c100f  add eax, ecx
  00007ff6`3f4c1011  pop rcx
  00007ff6`3f4c1012  ret  ; -> instruction-tracer-fixture.exe!drive+0x1c

trace ended: original stack frame returned
instructions: 8
```

A source heading appears only where the location changes, so straight-line code stays dense.

**The annotations are the point.** They are derived from where the CPU actually went next, not from
decoding the branch target — so they are exact even where static disassembly has to guess:

```
je   0x1120342a        ; taken -> mymodule.exe!zero_path
jae  0x11203500        ; not taken
call rax               ; -> allocator.dll!allocate+0x20
jmp  [rcx+0x18]        ; -> foo::implementation
ret                    ; -> caller+0x91
```

`; taken` / `; not taken` appear only for conditional branches, where there was a choice. For calls,
jumps and returns the interesting part is only ever *where it landed*.

Bytes that fail to decode print as hex in parentheses rather than being dropped.

## How it works

At each entry-breakpoint hit:

1. Rewind `rip` past the `int3` and restore the displaced byte.
2. Warm-up hit (`<= --skip`) → single-step that one instruction, re-arm, continue.
3. Otherwise capture the entry state and start stepping.

The entry breakpoint is the natural place to read the return address: the prologue has not run, so
`[rsp]` *is* it — no unwind info required. `--until-return` then stops at
`rip == return_rip && rsp >= entry_rsp + 8`. The `rsp` guard is what rejects a recursive call
returning to the same address at a deeper frame.

The trap flag is per-thread, and Windows freezes the debuggee while the debugger handles an event,
so other threads cannot perturb the stepping and are never suspended. They do keep running between
continues, so their stdout interleaves with the trace.

Collection stays lean: the loop records only `rip`, the raw bytes and (optionally) registers, and
detects syscalls by raw byte match. Disassembly and symbolization happen afterwards — a PDB lookup
per instruction would cost more than the tracing.

Disassembly is [Zydis](../../extern/zydis/), decoded in-process from the recorded bytes. It is
fetched on demand (`uv run extern/zydis/fetch-zydis.py`, which `dev.py` runs per configure) rather
than committed: the amalgamated source is ~12 MB of generated instruction tables.

## Limits

- **Windows x64 only** — Win32 debug API and dbghelp throughout.
- **Needs PDBs.** A `release-*` preset degrades to raw addresses with no symbols or source. Use a
  `relwithdebinfo-*` build.
- **No inline frames.** The stack is physical frames only; a heavily inlined caller shows as one
  frame. `SymQueryInlineTrace` would fix this.
- **Recursion during a trace is not counted.** The breakpoint stays unarmed for the duration of a
  trace, so a recursive re-entry is stepped as ordinary instructions rather than starting a new one.
- **`--stop-at-syscall` rarely fires.** Raw `syscall` lives in ntdll, not in user code, so a trace
  with a big budget usually walks into ntdll stubs long before reaching one. A "stop on leaving the
  module" condition is probably what you actually want; it does not exist yet.
- **`--skip` is linear** in breakpoint hits, at roughly 10–50µs each — `--skip 1000000` takes ~30s.
- **`popfq` / `iretq` can clear the trap flag** mid-trace and the debuggee runs away. Rare in user
  code. A backstop breakpoint at the return address would fix it.
- **Anti-debug targets** will see `IsDebuggerPresent`.
- Only the thread that hit the breakpoint is traced.

## Tests

Two layers, split by what they need:

- `uv run dev.py test instruction-tracer-test` — the pure parts: CLI and target-spec parsing, Zydis
  decoding, the module registry, and output formatting. Cross-platform-shaped and fast.
- `uv run tools/instruction-tracer/self-test.py` — the real thing: builds the tracer and
  `fixture/main.cc`, traces it, and checks the results. Windows-only and needs PDBs, which is why it
  is not part of the `dev.py test` sweep.

The self-test's assertions are optimizer-independent by construction. They never pin a codegen shape,
only what `__declspec(noinline)` and `volatile` guarantee — the call happens, the frame exists, the
function returns — plus one relation *between* runs that no optimizer can break: three traces of the
same function must retire identical addresses. That one is the load-bearing check; a missed step, a
stale breakpoint or a botched re-arm all break it.

## Layout

```
src/instruction-tracer/
  cli/        options + target-spec parsing
  debug/      the Win32 half: event loop, breakpoint, stepping, dbghelp, enrichment
  decode/     Zydis wrapper
  report/     source lookup + formatting
```

`debug/trace_record.hh` is the seam: plain data, no `<Windows.h>`. `decode/` and `report/` consume
it and never see the debug API, which is what lets them be unit-tested without a debuggee.
