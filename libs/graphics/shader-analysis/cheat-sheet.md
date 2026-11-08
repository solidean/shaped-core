# shader-analysis cheat sheet

What a shader binary costs, read off the binary alone.
Namespace `shan`; headers included by full path from `src/`.

> **The metrics compare two modules of one entry point; they model no GPU.**
> [readme.md](readme.md) is the front door.

How to read this: each block leads with the include; one symbol per line with a trailing comment giving the return type or the intuition.
Format conventions live in [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

---

**Recording domain:** `shan`.
`shan.analyze` scopes each analysis.

## Operation kinds

```cpp
#include <shader-analysis/op_kind.hh>

shan::op_kind::float_arithmetic;          // ... load, store, image, phi, control, declaration: what a GPU spends on it
shan::spirv_kind_of(opcode);              // -> op_kind of a SPIR-V opcode
shan::name_of(kind);                      // -> "float_arithmetic"
shan::latency_of(kind);                   // -> i32, rough cycles of an add: memory 16, extended 4, arithmetic 1
shan::k_op_kind_count;                    // the size of a per-kind table
```

## Metrics

```cpp
#include <shader-analysis/metrics.hh>

auto const m = shan::analyze(module).value();   // a babel::spirv::module or babel::llvm37::module; a malformed body is an error
shan::analyze_binary(bytes);              // a SPIR-V module or a DXIL container, told apart by its first bytes
m.total;                                  // function_metrics summed, max_live_words and critical_path the worst
m.functions;                              // one per OpFunction, named by OpName or %id
m.entry_points;                           // the OpEntryPoint names
f.instructions;                           // what runs: no labels, merges, lines or declarations
f.count_of(shan::op_kind::load);          // -> i32
f.blocks; f.loops; f.selections; f.calls;  // DXIL: a loop is a header back edges reach, a selection a conditional branch or switch
f.variable_words;                         // the function's own variables, in 32-bit words
f.max_live_words;                         // register pressure: the most words of SSA values live at once
f.max_live_vector_words;                  // the same of values that differ across a wave: what VGPRs hold
f.max_live_scalar_words;                  // the same of values the wave shares: what SGPRs may hold
f.critical_path;                          // the longest dependent chain in one block, in latency_of units
shan::to_text(m);                         // -> cc::string, one line a metric
```

## Cost

```cpp
#include <shader-analysis/cost.hh>

shan::compute_cost(f, shan::default_cost_model());   // -> f64, lane-cycles of an fma if compute bound
shan::xe2_cost_model(); shan::rdna2_cost_model();   // rates measured on the B580 and the Radeon; default is xe2
shan::cost_class::transcendental;          // free, alu, int_multiply, int_divide, conversion, transcendental, memory, control
f.executed_class_lanes[isize(c)];          // lanes per class, as often as a wave runs them
f.counted_loops;                           // loops whose counter states its trips; the rest run k_assumed_loop_trips
shan::cost_model{.per_lane = {...}};       // a GPU's rate per class; memory is never counted
```

## Diff

```cpp
#include <shader-analysis/diff.hh>

auto const d = shan::diff(a, b);          // -> cc::vector<difference>, the largest relative difference first
d[0].name; d[0].a; d[0].b;                // "max_live_words", and each module's value
d[0].ratio();                             // a over b; 1 where both are 0, infinite where only b is
shan::to_text(d);                         // -> cc::string, one line each
```

## Gotchas

- **An operand counts as a use only where it names a value of the function.**
  SPIR-V does not mark literals; shan knows the literal positions of the opcodes a body is made of, and a literal that happens to equal a value's id elsewhere would count.
- **A `NonSemantic.*` extended instruction is a declaration**, so debug info adds no operations.
- **A switch's cases are read as 32-bit literals**, which a 64-bit selector's are not.
- **Live words are in the module's own order, and a driver reorders before it allocates.**
  AMD gives the convolution 38 vector registers where shan counts 79 vector words, on both routes alike; compare two modules, never a module with a register file.
- **The compute cost is an estimate for two modules of one entry point.**
  A branch the wave shares runs each arm at its share, one it does not both in full, a loop's guard and its body's stay in full.
  Its rates are `pipeline-stats throughput`'s, measured on two GPUs; a multiply and its add count two where both run one fma.
  A loop's counter, compare and branch count every trip, which a driver that unrolls it does not pay; an integer multiply by a power of two is a shift.
- **Uniformity does not follow memory.** A load from a function's own variable is varying whatever was stored, and one from a buffer at a shared address is scalar.
