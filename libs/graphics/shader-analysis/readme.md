# shader-analysis

What a shader binary costs, read off the binary alone: operations by kind, register pressure, critical paths, and the difference between two modules of one entry point.
Namespace `shan`.
Depends on **clean-core** and **babel-data**, whose readers parse the formats as they are stored; this library says what they mean.

```cpp
#include <babel-data/shader/spirv.hh>
#include <shader-analysis/diff.hh>
#include <shader-analysis/metrics.hh>

auto const native = shan::analyze(babel::spirv::read(native_words).value()).value();
auto const dxc = shan::analyze(babel::spirv::read(dxc_words).value()).value();
for (auto const& d : shan::diff(native, dxc))      // worst first
    cc::println("{} {} {}", d.name, d.a, d.b);
```

## What it is for

SGL's native route is held to DXC's output, and an operation count stopped explaining what is left between them.
Two modules with the same arithmetic can still differ in what a driver cares about: how many values are live at once, how many phis and blocks carry them, how long a chain of dependent operations is.
shan measures those, so a comparison names where two modules part rather than only how large each is.

**The metrics are estimates for comparing two modules of one entry point, not a model of a GPU.**
Register pressure is the most 32-bit words of SSA values live at one point, a bool taking one and a logical pointer none.
It is split too: vector words are values that differ across a wave, which a GPU keeps in vector registers, and scalar words the values a wave shares.
The compute cost is what the module would spend if it were compute bound: each operation's lanes at its class's rate, as often as a wave runs it.
A loop runs the trips its counter states, or `k_assumed_loop_trips`, and a branch both arms where the wave does not share its condition.
Memory is left out, since two routes of one entry point read and write the same footprint.
The critical path is the longest dependent chain inside one block, weighted by a rough latency per kind (`latency_of`).
A driver allocates and schedules better or worse than that, but two modules of one entry point it treats alike.

## Formats

- **SPIR-V**, through `babel::spirv`; its loops and selections are its merges.
- **DXIL**, through `babel::dxil`, `babel::llvm` and `babel::llvm37`; a `dx.op` call is the kind its operation class is.
  A loop is a header that edges back from the blocks it dominates reach, however many there are.

`shan::analyze_binary` takes either and tells them apart by their first bytes.

## Command line

```bash
uv run dev.py run shan -- analyze module.spv       # the totals, then each function
uv run dev.py run shan -- diff native.spv dxc.spv  # every total the two differ in, worst first
uv run dev.py run shan -- cost module.spv          # the compute cost under each GPU's measured rates
```

`native-parity.py diff` leaves both routes' binaries in its output folder, which is where a pair to compare comes from: `.spv` and `.dxil` alike.

## Building & testing

```bash
uv run dev.py test shader-analysis-test
```

## More

- [cheat-sheet.md](cheat-sheet.md) — the API at a glance.
- [native-parity.md](../shaped-graphics-language/docs/native-parity.md) — how SGL's binaries measure against DXC's.
