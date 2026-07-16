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
| `--instructions <n>` | `100` (`100000` under `--stats`) | Max retired instructions per trace. |
| `--until-return` | on | Stop once the entry frame returns. |
| `--stop-at-syscall` | on | Stop before executing a syscall, rather than stepping into the kernel. |
| `--stack` | on | Print the stack at entry. |
| `--source` | on | Annotate with source file/line and the source text. |
| `--register-diffs` | off | Dump the registers at entry, then show what each instruction changed. See below. |
| `--stats` | off | Print a per-symbol table instead of the trace. See below. |
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

### `--register-diffs`

The full state at entry, then only what each instruction changed:

```
registers:
  rax=0x00000000000000c6  rcx=0x0000000000000064  rdx=0x0000000000000001  rbx=0x00000000000039a0
  rsp=0x0000005c3f58fe48  rbp=0x0000000000000000  rsi=0x0000000000000064  rdi=0x0000000000003a66
  r8 =0x000001ca585e0a00  r9 =0x00007ff7f90c0298  r10=0x00002660fad4cd66  r11=0x0000005c3f58fe48
  r12=0x0000000000000000  r13=0x0000000000000000  r14=0x0000000000000000  r15=0x0000000000000000
  rflags=0x00000293 [CF AF SF]

  00007ff7`f90c1000  push rax        ; rsp=0xb3eb2ffac0
  00007ff7`f90c100c  mov eax, [rsp]  ; rax=0x1
  00007ff7`f90c100f  add eax, ecx    ; rax=0x65 CF=0 PF=1 AF=0 SF=0
  00007ff7`f90c1012  ret             ; -> fixture.exe!drive+0x1c  ; rsp=0xb3eb2ffad0
```

The dump is what makes the diffs readable: `rcx=0x64` says what rcx *became*, never what it was.

Flags print by name and new value, because `rflags=0x293` answers nothing. Only the status flags the
code computes with are shown — `CF PF AF ZF SF DF OF`. **`TF` is excluded on purpose:** it is the
trap bit the tracer sets to single-step, so reporting it would describe the debugger rather than the
debuggee. `IF` and `RF` are system state the traced code does not author either.

Snapshots are sampled *before* each instruction, and one more is recorded after the last one retires
— otherwise a trailing `ret`'s `rsp` move would be invisible, which is exactly the instruction you
are usually looking at.

### `--stats`

Answers "where did the instructions go" without reading the trace. One row per symbol, sorted by
instruction count descending, aggregated over every recorded trace:

```
  self  atomics  slow  calls d/i  mem r/w  br (taken)  symbol
   109        2     0        8/0    20/22       9 (0)  clean-core-test.exe!`anonymous namespace'::single_lazy_probe
    84        2     0        3/1    12/10      10 (5)  clean-core-test.exe!cc::async_node_base::poll
    78        0     0        0/1     13/5       7 (3)  clean-core-test.exe!cc::poly_node_allocation::~poly_node_allocation
     9        0     0        1/0      2/1       0 (0)  clean-core-test.exe!cc::detail::unique_function_invoke
  ---------------------------------------------------
   797       10     0       24/5   142/92     77 (34)  total (1 trace)
```

| column | meaning |
|---|---|
| `self` | Instructions charged to the function **containing** them, so a callee's work never lands on its caller. Not cumulative. |
| `atomics` | Locked read-modify-writes — a `lock` prefix, or an `xchg` against memory (which locks implicitly). The highest instruction-to-cycle ratio on the table: 10 of 797 instructions above are ~43% of the cycles. |
| `slow` | Instructions that are categorically not single-cycle. Usually 0; when it is not, a footer names them. See below. |
| `calls d/i` | Direct / indirect. An indirect call is a vtable, `function_ref` or `unique_function` hop — what you are hunting when you ask why something did not inline. |
| `mem r/w` | Instructions with an explicit memory operand they read / write. Finds pointer chases and RFO-heavy zeroing. Implicit stack traffic (`push`/`pop`) does not count, or a prologue would drown out the signal. |
| `br (taken)` | Conditional branches, and how many were taken. A mispredict candidate is a hot branch near half taken. |

Template arguments are stripped (`cc::vector<int>::push_back` → `cc::vector::push_back`) — the real
names run to 300+ chars — so two instantiations of one function share a row.

#### The `slow` column

Every column above assumes one instruction ≈ one cycle. `slow` is where that assumption is *known* to
break: `idiv`, `div`, float divide and `sqrt`, `cpuid`, `rdtsc`, `rdrand`, fences, `pause`,
`rep`-prefixed string ops, gathers/scatters, x87 transcendentals. Tens to hundreds of cycles each,
sitting in a stream where everything else is one.

They are named rather than only counted, because which one it is *is* the finding:

```
slow ops (tens of cycles each — the instruction count does not show these)
  scasd  x12  ntdll.dll!RtlCompareMemoryUlong
  stosq   x3  ntdll.dll!RtlSetExtendedFeaturesMask
  divss   x1  clean-core-test.exe!std::unordered_map::_Insert_or_assign
```

That last line is the point of the column: `std::unordered_map` does a **float divide on every
insert** — the `size() / bucket_count() > max_load_factor()` check — and nothing about an instruction
count would ever show it. (`cc::map` masks power-of-two buckets instead, which is why it has no such
row.) The usual way one appears is a `%` on a non-power-of-two, or profiling code built out of
`rdtsc`. A `pause` means a spinlock is actually spinning.

**It is not a cost model, on purpose.** Exact latencies are microarchitecture-specific, so nothing
here estimates or weighs — membership is the whole claim: *the instruction count will mislead you
here, go look*. An all-zero column is a real result too: it says the count is a fair proxy.

The cost it **cannot** see is the one that usually matters. A `mov` that misses to DRAM is 200+
cycles and is indistinguishable from an L1 hit. This finds landmines in the opcode stream, not where
the time went.

`--stats` raises the `--instructions` default to 100000: a trace cut short by the budget produces a
silently wrong table, and 100 truncates anything worth tabling. An explicit `--instructions` still
wins, and a table built from a truncated trace says so loudly. Note that single-stepping costs a
debug-event round trip per instruction, so a genuinely 100k-instruction trace is slow.

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
- **`--register-diffs` is GPRs + rflags only — no XMM/YMM.** Vectorized code moves through `xmm0-15`
  invisibly, so a trace of it shows the loads and none of the arithmetic. TODO: capture `Xmm0-15`
  from `CONTEXT` (they are already in the struct we read) and diff them like the GPRs; the open
  question is rendering 128 bits per register without swamping the line, which probably means
  printing only the changed lanes and only on request.
- **The syscall stop has no trailing register snapshot.** Every other stop records the state the last
  instruction left behind; the syscall gate is recorded but deliberately never stepped, so its effect
  is unknown rather than missing.
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
