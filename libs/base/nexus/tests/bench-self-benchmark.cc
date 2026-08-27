#include <clean-core/container/vector.hh>
#include <clean-core/record/stat.hh>
#include <nexus/bench/bench.hh>
#include <nexus/bench/run.hh>
#include <nexus/test.hh>

// The harness measuring itself.
//
// These are the numbers every warning in the engine is compared against, so they are worth being able to read directly
// rather than only through the calibration the engine does privately.
// They are also the first real BENCHMARKs in the repo, which makes them the working example of the macro.
//
//   uv run dev.py benchmark "nx::bench"
//
// Most loops here use the void(isize) form, and that is not incidental.
// These bodies are a fraction of a nanosecond, so the harness's own per-iteration cost would be most of what was
// measured in any other form — which is exactly the situation the overhead warning exists to point at.

using namespace cc::primitive_defines;

namespace
{
u64 mix(u64 x)
{
    return x * 2654435761u + 12345u;
}
} // namespace

BENCHMARK("nx::bench - the barriers")
{
    // The floor: a loop whose body cannot be deleted and does nothing else.
    // Every other row here is read against this one.
    auto counter = u64(0);
    nx::bench::run("empty loop",
                   [&](isize count)
                   {
                       for (auto i = isize(0); i < count; ++i)
                           nx::bench::sink(i);
                       nx::bench::sink(counter);
                   });

    // `sink` IS `keep` with the result dropped, so a second guard in the body is what actually shows their marginal
    // cost.
    // Read this against "empty loop" rather than in absolute terms.
    nx::bench::run("two guards per iteration",
                   [&](isize count)
                   {
                       for (auto i = isize(0); i < count; ++i)
                           nx::bench::sink(nx::bench::keep(i));
                       nx::bench::sink(counter);
                   });

    // The guard in a serial accumulator chain, which is how a benchmark body usually ends up using it.
    // Slower than the rows above by the chain rather than by the guard: `asm volatile` blocks the reassociation that
    // would otherwise let the loop run several accumulators at once.
    nx::bench::run("guarded accumulate",
                   [&](isize count)
                   {
                       for (auto i = isize(0); i < count; ++i)
                           counter += nx::bench::keep(i);
                       nx::bench::sink(counter);
                   });

    // Zero instructions, so this must land on the floor rather than above it.
    // A row here that is measurably slower than "empty loop" means the barrier stopped being free.
    nx::bench::run("compiler_barrier",
                   [&](isize count)
                   {
                       for (auto i = isize(0); i < count; ++i)
                       {
                           nx::bench::sink(i);
                           nx::bench::compiler_barrier();
                       }
                       nx::bench::sink(counter);
                   });

    // The one construct here that costs real time, and the reason a benchmark pauses around it.
    nx::bench::run("sink over a span (a clobber, not a read)",
                   [&](isize count)
                   {
                       auto buffer = cc::vector<u64>::create_defaulted(512);
                       for (auto i = isize(0); i < count; ++i)
                       {
                           buffer[i % 512] = u64(i);
                           nx::bench::sink(cc::as_bytes(buffer));
                       }
                   });
}

BENCHMARK("nx::bench - reading the clock")
{
    // Why batching is not optional: one pair of readings costs more than most bodies, so a nanosecond-scale
    // measurement has to amortize it over a batch rather than time an iteration directly.
    nx::bench::run("one cycle counter read",
                   [&](isize count)
                   {
                       for (auto i = isize(0); i < count; ++i)
                           nx::bench::sink(cc::current_cycles());
                   });

    nx::bench::run("a pause/resume pair",
                   [&](nx::bench::iteration& it)
                   {
                       it.pause();
                       it.resume();
                       nx::bench::sink(it.index());
                   });
}

BENCHMARK("nx::bench - the measured work itself")
{
    // A body large enough that the harness is genuinely negligible, which is what the overhead warning's threshold is
    // about: at 2% the floor of ~0.1 ns clears anything above roughly six nanoseconds.
    auto acc = u64(0);

    nx::bench::run("32 mixes, void(isize)",
                   [&](isize count)
                   {
                       for (auto i = isize(0); i < count; ++i)
                       {
                           for (auto k = 0; k < 32; ++k)
                               acc = mix(acc);
                       }
                       nx::bench::sink(acc);
                   });

    // The same work through the handle form, which is what an author writes by default.
    // The gap between these two rows IS the harness's per-iteration cost, measured rather than calibrated.
    nx::bench::run("32 mixes, void(iteration&)",
                   [&](nx::bench::iteration& it)
                   {
                       for (auto k = 0; k < 32; ++k)
                           acc = mix(acc);
                       nx::bench::sink(acc);
                       it.items(32);
                   });
}
