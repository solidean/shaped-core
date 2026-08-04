# instruction-tracer

Records what optimized x64 code **actually executed** — the retired instructions of a real invocation, with the branches it really took and the indirect calls it really made.

`dev.py assembly search` / `show` answer the static question: what *might* this code do.
This answers the dynamic one — it launches a program under the Win32 debug API, breakpoints a symbol, skips the warm-up hits, then single-steps one invocation and prints it.
Where `trace` sits in that workflow is the [disassembly guide](../../docs/guides/disassembly.md)'s subject.
Driving it inside a session is the [disassembly skill](../../.claude/skills/disassembly/SKILL.md)'s.

It is deliberately not a general debugger: the narrow scope is what keeps this a few hundred lines of debug loop rather than DynamoRIO or Pin.

**Windows x64 only.**
Built by default in a top-level build (`SC_BUILD_TOOLS`), skipped when shaped-core is consumed via `add_subdirectory`.
Windows ARM64 skips it too — tool, tests and Zydis alike: x64 is structural here (the x86-64 `CONTEXT` registers, an x86 decoder), so the tool is absent there rather than ported.

## Quick start

Drive it through `dev.py`, which builds both binaries and resolves their paths — never construct build paths by hand:

```bash
uv run dev.py assembly trace --target clean-core-test \
    --symbol "cc::async_node_base::schedule" \
    -- "async - basic"
```

Everything after `--` goes to the debuggee verbatim — above, a nexus test-name filter, so the process runs just the test that exercises the code you care about.

### Another project's binary

Nothing in the tracer knows about shaped-core — it takes a path to an `.exe` and reads its PDB at run time.
`--exe` exposes that through `dev.py`, in place of `--target`:

```bash
uv run dev.py assembly trace --exe D:/proj/out/bin/app.exe \
    --symbol "render::draw" --skip 50 --stats -- --scene foo.json
```

The traced binary is yours to build; `dev.py` still builds and locates the tracer itself.
The debuggee runs in the exe's own directory, so its DLLs and relative data paths resolve — `dev.py assembly`'s `--cwd PATH` overrides that.
A relative `--exe` / `--cwd` / `--html` is relative to *your* current directory, not the repo.
See [Other projects](../../docs/guides/disassembly.md#other-projects) for the `search`/`show` half.

## Usage

`dev.py assembly trace` mirrors the flags below, with a few differences.
`--target` there names a *build target* of this repo, and `--exe` takes an arbitrary path instead — exactly like this tool's `--exe`.
The tracer's own `--target` spec is spelled `--spec` there, since the name is taken.
There is no `--mca`: `dev.py` finds `llvm-mca` itself and passes it whenever it resolves.
It also adds `--preset` and `--no-build`, which mean nothing to this tool.

```
uv run dev.py assembly trace (--target <build-target> | --exe <path>)
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

`--target` accepts `foo::bar`, `0x7ff6...`, `mod.exe!foo::bar`, and `mod.exe+0x3410`.
Module names are case-insensitive and a bare stem works (`mymodule` finds `mymodule.exe`).
`!` binds tighter than `+`, so `mod.dll!operator+` parses as you would want.

Prefer `mod.exe+0x3410` over `--address` when scripting: it is immune to ASLR, which re-bases each image once per boot.

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
| `--instructions <n>` | `100` (`100000` when a non-trace section is on) | Max retired instructions per trace. |
| `--until-return` | on | Stop once the entry frame returns. |
| `--stop-at-syscall` | on | Stop before executing a syscall, rather than stepping into the kernel. |
| `--sections <list>` | `trace` | Comma-separated output sections, all from one capture. See [Sections](#sections). |
| `--html <path>` | off | Write a self-contained HTML report. See [HTML export](#html-export). |
| `--mca <path>` | off | Path to `llvm-mca`; enables the [`timing`](#timing-llvm-mca) cost model. `dev.py` resolves it automatically. |
| `--mca-cpu <name>` | host | The micro-arch `llvm-mca` models (`-mcpu`). Empty means the host CPU (`-mcpu=native`). |
| `--stack` | on | Print the stack at entry (trace section). |
| `--source` | on | Annotate with source file/line and the source text (trace section). |
| `--register-diffs` | off | Dump the registers at entry, then show what each instruction changed. See below. |
| `--stats` | off | Shortcut for `--sections stats`. |
| `--memory-regions <list>` | `heap,stack` | Which address regions the memory sections show. See [Memory](#memory). |
| `--memory-instruction-addresses` | off | Annotate the memory and cacheline views with the accessing instruction. |
| `--terminate-after-traces` | on | Kill the debuggee once the last trace lands. |

Every boolean has a `--no-` form (`--no-source`). `-h` / `--help` prints all of this.

Choosing `--skip` well — warm the probe, then skip past the cold hits — is workflow rather than reference; the [disassembly skill](../../.claude/skills/disassembly/SKILL.md) carries that discipline.

Output is colored when stdout and stderr are both terminals; `--colored` / `--plain` override, and `NO_COLOR` / `FORCE_COLOR` are honored (`NO_COLOR` wins).
Same policy as `dev.py`, and driving it via `dev.py assembly trace` makes dev.py's choice authoritative.

Exit codes: `0` traced something, `1` bad usage or a failed `--html` write, `2` nothing was traced — the target was unknown, ambiguous or never entered, or the debuggee never launched.

## Output

### Sections

The output is a set of sections you combine with `--sections <list>`; the default is `trace` alone.
Every selected section is rendered from **one** capture, since the memory data cannot be reliably reproduced across separate runs.
The names:

| section | what it prints |
|---|---|
| `trace` | the retired-instruction trace (below); honors `--source` / `--register-diffs` |
| `stats` | the per-symbol instruction table (`--stats` is a shortcut for this) |
| `memory` | the raw chronological memory-access list — see [Memory](#memory) |
| `cachelines` | memory accesses bucketed by cacheline |
| `memory-stats` | the per-symbol memory table |
| `timing` | the `llvm-mca` cost model (needs `--mca`) — see [Timing](#timing-llvm-mca) |

`--sections trace,memory,memory-stats` prints three blocks in that fixed order.
Naming a section replaces the default trace, so `--stats` alone prints only the table — pass `--sections trace,stats` for both.
Any non-trace section raises the `--instructions` default to `100000`, since a trace cut short by the budget makes every aggregate silently wrong.
An explicit `--instructions` still wins, and a table built from a truncated trace says so loudly.

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

**The annotations are the point.**
They are derived from where the CPU actually went next, not from decoding the branch target — so they are exact even where static disassembly has to guess:

```
je   0x1120342a        ; taken -> mymodule.exe!zero_path
jae  0x11203500        ; not taken
call rax               ; -> allocator.dll!allocate+0x20
jmp  [rcx+0x18]        ; -> foo::implementation
ret                    ; -> caller+0x91
```

`; taken` / `; not taken` appear only for conditional branches, where there was a choice.
For calls, jumps and returns the interesting part is only ever *where it landed*.

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

Flags print by name and new value, because `rflags=0x293` answers nothing.
Only the status flags the code computes with are shown — `CF PF AF ZF SF DF OF`.
`TF`, `IF` and `RF` are excluded: `TF` is the trap bit the tracer sets to single-step, and the other two are system state the traced code does not author.

Snapshots are sampled *before* each instruction, and one more is recorded after the last one retires.
Otherwise a trailing `ret`'s `rsp` move would be invisible, which is exactly the instruction you are usually looking at.

### `--stats`

Answers "where did the instructions go" without reading the trace.
One row per symbol, sorted by instruction count descending, aggregated over every recorded trace:

```
  self  atomics  slow  calls d/i  mem r/w  br (taken)  symbol
   109        2     0        8/0    20/22       9 (0)  clean-core-test.exe!`anonymous namespace'::single_lazy_probe
    84        2     0        3/1    12/10      10 (5)  clean-core-test.exe!cc::async_node_base::poll
    78        0     0        0/1     13/5       7 (3)  clean-core-test.exe!cc::poly_node_allocation::~poly_node_allocation
     9        0     0        1/0      2/1       0 (0)  clean-core-test.exe!cc::impl::unique_function_invoke
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

Template arguments are stripped (`cc::vector<int>::push_back` → `cc::vector::push_back`) — the real names run to 300+ chars — so two instantiations of one function share a row.

#### The `slow` column

Every column above assumes one instruction ≈ one cycle.
`slow` is where that assumption is *known* to break.
The members: `idiv`, `div`, float divide and `sqrt`, fences and `clflush`, `pause`, `rep`-prefixed string ops, gathers/scatters, x87 transcendentals.
Plus the serializing and timing reads: `cpuid`, `rdtsc`, `rdtscp`, `rdpmc`, `rdrand`, `rdseed`, `xgetbv`, `wbinvd`.
Tens to hundreds of cycles each, sitting in a stream where everything else is one.

They are named rather than only counted, because which one it is *is* the finding:

```
slow ops (tens of cycles each — the instruction count does not show these)
  scasd  x12  ntdll.dll!RtlCompareMemoryUlong
  stosq   x3  ntdll.dll!RtlSetExtendedFeaturesMask
  divss   x1  clean-core-test.exe!std::unordered_map::_Insert_or_assign
```

That last line is the point of the column: `std::unordered_map` does a **float divide on every insert**, the `size() / bucket_count() > max_load_factor()` check.
Nothing about an instruction count would ever show it.
(`cc::map` masks power-of-two buckets instead, which is why it has no such row.)
The usual way one appears is a `%` on a non-power-of-two, or profiling code built out of `rdtsc`.
A `pause` means a spinlock is actually spinning.

**It is not a cost model, on purpose.**
Exact latencies are microarchitecture-specific, so nothing here estimates or weighs — membership is the whole claim: *the instruction count will mislead you here, go look*.
An all-zero column is a real result too: it says the count is a fair proxy.

The cost it **cannot** see is the one that usually matters.
A `mov` that misses to DRAM is 200+ cycles and is indistinguishable from an L1 hit, so this finds landmines in the opcode stream rather than where the time went.
Covering that blind spot is what the [memory sections](#memory) are for.

Note that single-stepping costs a debug-event round trip per instruction, so a genuinely 100k-instruction trace is slow.

### Timing (llvm-mca)

Where `slow` (above) only flags that the instruction count *will* mislead, `timing` puts a number on it.
It feeds the retired instruction stream to [`llvm-mca`](https://llvm.org/docs/CommandGuide/llvm-mca.html), a static issue/execute/retire model.
Out come µops, latency and throughput per instruction, plus a block summary, per-port pressure and a bottleneck breakdown.
It needs `--mca <path>`; through `dev.py assembly trace` the tool is resolved automatically, and passed for `--html` too.

```
timing  trace 1/1   model znver4
  IPC 2.36   block RThroughput 3.33   cycles 339   uops 800   dispatch 6   iters 100
  bottleneck: register-dep 292c  data-dep 292c  memory-dep 0c  resource 293c  limited by Zn4LSU (293c)
  addr              uops  lat   @ret  text
  00007ff7`677e1000     1    1     @3  push rax
  00007ff7`677e1008     1    5     @7  mov ecx, [rsp+0x04]
  00007ff7`677e100f     1    1     @9  add eax, ecx
  00007ff7`677e1012     1    5     @9  ret
```

`@N` is the cycle the instruction retired in.
The model is the **host CPU** by default (`-mcpu=native`), with a graceful fallback to a baseline if native is unavailable; `--mca-cpu <name>` overrides it.
The [HTML export](#html-export) is where this shines: a `timing` toggle, the per-instruction `@retire` cycle, and block-summary and port-pressure/bottleneck side boxes.
Plus a full-width **waterfall**, the classic mca pipeline diagram.

**Read it honestly — it is a static model.**

- **Whole-stream, not loop-detected.** We feed the whole retired stream and let mca aggregate it over its default 100 iterations, which models a *frequently invoked* function near steady state.
  The summary therefore assumes the block loops; it is not a one-shot cost.
- **Steady-state summary vs single-pass waterfall.** The block summary is the 100-iteration aggregate, while the `@retire` cycles and the waterfall come from iteration 0, a single pass.
  Don't cross-read them.
- **Blind to caches, the same landmine as [`slow`](#the-slow-column).** It models the pipeline, not the memory system, so a `mov` that misses to DRAM costs 1 cycle here too.
  `timing` tells you what the *scheduler* would do with this instruction mix; it cannot tell you where the wall-clock time went.

Instructions `llvm-mca` cannot parse — it walks into ntdll, or hits an encoding its model lacks — are dropped and render with blank timing.
If the survivors cannot be reconciled to the trace, the per-instruction column and waterfall are suppressed for that trace rather than mis-attached; the block summary and port pressure still show.

## Memory

The one cost the instruction stream hides is where the data went — the DRAM miss [`slow`](#the-slow-column) cannot see either.
The memory sections resolve every memory operand to its **effective address** at run time, from the register snapshot taken before each instruction, and show what the invocation actually touched.
Three views, all from the same capture:

- **`memory`** — the raw list, one line per access in execution order: address, size, read/write, the region, and the symbol it hit.
  With `--memory-instruction-addresses`, the accessing rip is prefixed.
- **`cachelines`** — accesses bucketed into 64-byte lines, one line per touched cacheline in ascending order, with a blank line across a gap.
  It shows how many accesses hit the line, how many of its 64 bytes — the footprint, the "am I using the whole line or 8 bytes of it" signal — and the distinct symbols on it.
  This is the view for checking you access your data well.
- **`memory-stats`** — a per-symbol table (`acc`, `r/w`, distinct `lines`, `bytes` moved), grouped by the function *making* the accesses, like the instruction table.

```
=== cachelines ===
  000000ab`c17cfa00     9 acc  32/64 B  RW  ; …!itrace_fixture_touch   <- the frame's stack array
  000000ab`c17cfa40     3 acc  16/64 B  RW  ; …!itrace_fixture_touch

  00007ff6`5f531a40     2 acc   4/64 B  RW  ; …!itrace_global_counter  <- a global, named from the PDB
```

### Regions

Every address is classified into one of four regions, and `--memory-regions <list>` selects which the sections show (default `heap,stack`):

| region | what it is | default |
|---|---|---|
| `heap` | dynamic allocations and globals (a global keeps its name) | shown |
| `stack` | *another* function's stack — where a stack array passed around as a `span` lands, named with the frame's function | shown |
| `frame` | the current function's own stack: its locals, spills, and the return-address / saved-register machinery | hidden |
| `instructions` | code memory — the instruction fetch itself, so an I-cache footprint | hidden |

`frame` and `instructions` are off by default: they are the current function's own overhead and the code stream, which drown out the data accesses you are usually asking about.
Turn them on to see the whole picture (`--memory-regions heap,stack,frame,instructions`).

Region is what separates a genuine cross-function `stack` access — a stack buffer reached through a pointer, the case that is easy to get wrong — from a `frame` access that is just a local.
The frame boundaries are recovered by watching `call`/`ret` as the trace steps.

Requesting any memory section forces register capture on, since the effective addresses need it, so a memory run is a per-instruction snapshot heavier than a plain trace.

## HTML export

`--html <path>` writes the whole capture to a single self-contained `.html` file — CSS and JS inlined, no external requests — meant to be opened in a browser and shared as one artifact.
It is an output *format*, orthogonal to `--sections`: on its own it replaces the stdout rendering with a one-line `wrote <path> (<n> traces)`; combine it with `--sections` to get both.

Because a truncated or under-enriched export is misleading, `--html` **forces a full capture**: source, owner and memory enrichment plus register capture.
It takes the `100000` instruction budget too, and an explicit `--instructions` still wins.

The page is tabbed per trace, with a two-column layout.
On the left, the trace itself — toggles for inline source, register diffs, memory accesses and `timing`, and mnemonics linked to [felixcloutier.com](https://www.felixcloutier.com/x86/).
On the right, collapsible aggregate views: instruction stats, the three memory views driven by live region checkboxes, and a **source view**.
When `--mca` is available a **block summary** and a **port-pressure / bottleneck** box join them, under a collapse-all control.
The source view collects every line the trace touched, grows each by context, merges them into ranges, and renders them with syntax highlighting and an executed-line marker.
Hovering an executed line highlights the instructions that ran it, and vice versa.

When timing data is present and its alignment held, a second tab level appears below the per-trace tabs: **trace view**, the two-column layout, and **waterfall view**.
The waterfall is a full-width pipeline diagram whose rows are instructions and columns cycles, each bar segmented dispatch → execute → retire.
The [model's caveats](#timing-llvm-mca) apply to these views too.

```bash
uv run dev.py assembly trace --target clean-core-test \
    --symbol "cc::async_node_base::schedule" --html trace.html \
    -- "async - basic"
```

Source text is read from the build machine and embedded, so the file is self-contained but carries whatever source those paths resolved to.
Windows-only, and it needs PDBs like every other section.

## How it works

At each entry-breakpoint hit:

1. Rewind `rip` past the `int3` and restore the displaced byte.
2. Warm-up hit (`<= --skip`) → single-step that one instruction, re-arm, continue.
3. Otherwise capture the entry state and start stepping.

The entry breakpoint is the natural place to read the return address: the prologue has not run, so `[rsp]` *is* it — no unwind info required.
`--until-return` then stops at `rip == return_rip && rsp >= entry_rsp + 8`.
The `rsp` guard is what rejects a recursive call returning to the same address at a deeper frame.

The trap flag is per-thread, and Windows freezes the debuggee while the debugger handles an event, so other threads cannot perturb the stepping and are never suspended.
They do keep running between continues, so their stdout interleaves with the trace.

Collection stays lean: the loop records only `rip`, the raw bytes and (optionally) registers, and detects syscalls by raw byte match.
Disassembly and symbolization happen afterwards — a PDB lookup per instruction would cost more than the tracing.

Disassembly is [Zydis](../../extern/zydis/), decoded in-process from the recorded bytes.
It is fetched on demand (`uv run extern/zydis/fetch-zydis.py`, which `dev.py` runs per configure) rather than committed: the amalgamated source is ~12 MB of generated instruction tables.

## Limits

- **Windows x64 only** — Win32 debug API and dbghelp throughout.
- **Needs PDBs.** A `release-*` preset degrades to raw addresses with no symbols or source; use a `relwithdebinfo-*` build.
- **No inline frames.** The stack is physical frames only, so a heavily inlined caller shows as one frame.
  `SymQueryInlineTrace` would fix this.
- **`--register-diffs` is GPRs + rflags only — no XMM/YMM.** Vectorized code moves through `xmm0-15` invisibly, so a trace of it shows the loads and none of the arithmetic.
  TODO: capture `Xmm0-15` from `CONTEXT` (they are already in the struct we read) and diff them like the GPRs.
  The open question is rendering 128 bits per register without swamping the line, which probably means printing only the changed lanes and only on request.
- **Memory addresses skip the segment base.** A `gs`/`fs`-relative operand (TLS, the stack cookie's base) resolves to its offset alone, since the tracer does not read the segment base.
  A TLS access therefore lands at a small address rather than its real one.
  The common GPR-relative forms are exact.
- **Frame tracking misses tail calls.** Region classification recovers frame boundaries from `call`/`ret`, and a tail-call `jmp` into another function is not a call.
  Its accesses are charged to the caller's frame instead.
  Rare in the code this is aimed at.
- **The syscall stop has no trailing register snapshot.** Every other stop records the state the last instruction left behind.
  The syscall gate is recorded but deliberately never stepped, so its effect is unknown rather than missing.
- **Recursion during a trace is not counted.** The breakpoint stays unarmed for the duration of a trace, so a recursive re-entry is stepped as ordinary instructions rather than starting a new one.
- **`--stop-at-syscall` rarely fires.** Raw `syscall` lives in ntdll, not in user code, so a trace with a big budget usually walks into ntdll stubs long before reaching one.
  A "stop on leaving the module" condition is probably what you actually want; it does not exist yet.
- **`--skip` is linear** in breakpoint hits, at roughly 10–50µs each, so `--skip 1000000` costs tens of seconds.
- **`popfq` / `iretq` can clear the trap flag** mid-trace, and the debuggee then runs away.
  Rare in user code; a backstop breakpoint at the return address would fix it.
- **Anti-debug targets** will see `IsDebuggerPresent`.
- Only the thread that hit the breakpoint is traced.

## Tests

Two layers, split by what they need:

- `uv run dev.py test instruction-tracer-test` — the pure parts: CLI and target-spec parsing, Zydis decoding, the module registry, and output formatting.
  Cross-platform-shaped and fast.
- `uv run tools/instruction-tracer/self-test.py` — the real thing: builds the tracer and `fixture/main.cc`, traces it, and checks the results.
  Windows-only and needs PDBs, which is why it is not part of the `dev.py test` sweep.

The self-test's assertions are optimizer-independent by construction, and one of them is load-bearing for the whole tool.
`self-test.py` says which in its module docstring, and why in that check's own.

## Layout

```
src/instruction-tracer/
  cli/        options + target-spec parsing
  debug/      the Win32 half: event loop, breakpoint, stepping, dbghelp, enrichment, llvm-mca launch
  decode/     Zydis wrapper: instruction text, and memory effective-address + region classification
  report/     source lookup + formatting (trace, stats, memory views, and the llvm-mca timing model)
    html/     the HTML report's front end — app.js renders the embedded trace data, app.css styles it
```

`report/mca.{hh,cc}` is pure — asm builder, `-json` parse, alignment — and unit-tested against a checked-in fixture; `debug/mca_runner.cc` is the Win32 subprocess that feeds `llvm-mca`.
The JSON is read by a minimal in-tree reader (`report/json_reader.hh`) rather than babel-serializer's, which this tool does not depend on.

`report/html/` is not compiled: `embed-html-assets.py` turns `app.js` and `app.css` into a generated `html_assets.hh` at build time, which `html_export.cc` inlines into the page.
So the front end is edited in those two files, never in the generated header.

`debug/trace_record.hh` is the seam: plain data, no `<Windows.h>`.
`decode/` and `report/` consume it and never see the debug API, which is what lets them be unit-tested without a debuggee.
