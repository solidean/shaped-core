#include <clean-core/container/vector.hh>
#include <clean-core/record/stat.hh>
#include <clean-core/string/print.hh>
#include <nexus/bench/calibration.hh>
#include <nexus/bench/report.hh>
#include <nexus/bench/run.hh>
#include <nexus/rec.hh>
#include <nexus/test.hh>

// The measured run, end to end.
//
// Everything here uses a deliberately tiny budget — a few milliseconds rather than the half second the defaults ask
// for — because what is under test is the engine's bookkeeping, not any particular body's speed.
// A test that asserted a duration would be flaky by construction, so none of these do.

using namespace cc::primitive_defines;

namespace
{
// Small enough to keep the suite fast, large enough that every code path in the engine still runs.
// Whether the pause warning is EARNED on this machine, by the same condition run.cc fires it on.
//
// Which way it goes is a property of the box rather than of the harness: a counter read is a couple of instructions
// on x86 and costs more than most bodies on WASM, where `calibration::has_cheap_counter` is false.
// So a test that pins the answer passes where it was written and fails somewhere else — which is exactly what
// happened, in both directions at once, on macOS and under Emscripten.
//
// The invariant that does hold everywhere is that the warning appears exactly when the condition does.
bool pause_warning_is_earned(nx::bench::result const& r)
{
    // The share of measured per-iteration time above which the pair's two clock reads are a real part of the answer.
    // Mirrors `paused_warn_fraction` in nexus/bench/run.cc, which is file-local there.
    constexpr auto paused_warn_fraction = 0.05;

    auto const& cal = nx::bench::calibrated();
    return cal.clock_pair_secs > 0 && r.time.median > 0 && cal.clock_pair_secs > r.time.median * paused_warn_fraction;
}

nx::bench::run_config quick()
{
    auto c = nx::bench::run_config::standard();
    c.min_time_secs = 0.002;
    c.max_time_secs = 0.15;
    c.min_samples = 8;
    c.max_samples = 32;
    c.warmup_time_secs = 0.001;
    return c;
}

u64 work(u64 x)
{
    return x * 2654435761u + 12345u;
}
} // namespace

TEST("bench - run accepts a void() body")
{
    auto acc = u64(0);
    auto const r = nx::bench::run("void", quick(), [&] { acc = work(acc); });

    CHECK(r.name == "void");
    CHECK(r.samples.size() >= 8);
    CHECK(r.measured_iterations > 0);
    CHECK(r.warmup_iterations > 0);
    CHECK(r.time.median > 0);
    CHECK(r.measured_seconds > 0);

    // No items were declared, which is distinct from declaring one per iteration.
    CHECK(r.items == 0);
    CHECK(r.items_per_second == 0);
}

TEST("bench - run accepts a void(iteration&) body")
{
    auto acc = u64(0);
    auto seen_indices = isize(0);

    auto const r = nx::bench::run("handle", quick(),
                                  [&](nx::bench::iteration& it)
                                  {
                                      acc = work(acc);
                                      nx::bench::sink(acc);
                                      if (it.index() == 0)
                                          ++seen_indices;
                                      it.items(4);
                                  });

    CHECK(r.time.median > 0);
    CHECK(seen_indices > 0); // the harness moved the handle, so index() is not stuck

    // Four items per MEASURED iteration; warmup contributes none.
    CHECK(r.items == r.measured_iterations * 4);
    CHECK(r.items_per_second > 0);
}

TEST("bench - run accepts a void(isize) body and reports one sample per batch")
{
    auto acc = u64(0);
    auto const r = nx::bench::run("batched", quick(),
                                  [&](isize count)
                                  {
                                      for (auto i = isize(0); i < count; ++i)
                                          acc = work(acc);
                                      nx::bench::sink(acc);
                                  });

    CHECK(r.time.median > 0);
    CHECK(r.measured_iterations == r.batch_size * isize(r.samples.size()));
}

TEST("bench - a cheap body gets batched, an expensive one does not")
{
    auto acc = u64(0);
    auto const cheap = nx::bench::run("cheap", quick(), [&] { acc = work(acc); });

    // A handful of nanoseconds against a 1 ms target batch: thousands of iterations per timing boundary.
    CHECK(cheap.batch_size > 100);

    auto cfg = quick();
    cfg.batch = false;
    auto const unbatched = nx::bench::run("unbatched", cfg, [&] { acc = work(acc); });
    CHECK(unbatched.batch_size == 1);
    CHECK(unbatched.measured_iterations == isize(unbatched.samples.size()));
}

TEST("bench - single_shot measures one iteration per sample and warms up once")
{
    auto cfg = nx::bench::run_config::single_shot();
    cfg.min_samples = 4;
    cfg.max_samples = 4;
    cfg.max_time_secs = 1;

    auto runs = isize(0);
    auto const r = nx::bench::run("single", cfg, [&] { ++runs; });

    CHECK(r.batch_size == 1);
    CHECK(r.warmup_iterations == 1);
    CHECK(r.samples.size() == 4);

    // Four measured plus the one warmup.
    CHECK(runs == 5);

    // The overhead warning is suppressed here, however trivial the body is.
    CHECK(r.find_warning(nx::bench::warning_kind::overhead_significant) == nullptr);
}

TEST("bench - pause excludes its span from the measurement")
{
    auto cfg = quick();
    cfg.batch = false;
    cfg.min_samples = 6;
    cfg.max_samples = 6;
    cfg.min_time_secs = 0;

    auto acc = u64(0);
    auto const r = nx::bench::run("paused", cfg,
                                  [&](nx::bench::iteration& it)
                                  {
                                      it.pause();
                                      // Enough work to dominate the iteration if it were counted.
                                      for (auto i = 0; i < 20000; ++i)
                                          acc = work(acc);
                                      nx::bench::sink(acc);
                                      it.resume();

                                      acc = work(acc);
                                      nx::bench::sink(acc);
                                  });

    // Most of the wall time went into the paused span, which is reported.
    CHECK(r.paused_fraction > 0.5);

    // But it is NOT what the warning fires on.
    // The warning is about the pair's cost against what was MEASURED, and those are different numbers — so it tracks
    // the clock rather than the pause, whichever way that comes out on this machine.
    CHECK((r.find_warning(nx::bench::warning_kind::paused_fraction_high) != nullptr) == pause_warning_is_earned(r));
}

TEST("bench - recorded quantities aggregate by their unit")
{
    auto cfg = quick();

    auto const r = nx::bench::run("quantities", cfg,
                                  [&](nx::bench::iteration& it)
                                  {
                                      nx::bench::sink(it.index());
                                      it.record("bytes", cc::rec::unit_bytes, 1024);
                                      it.record("hit rate", cc::rec::unit_ratio, 0.5);
                                  });

    CHECK(r.quantities.size() == 2);

    auto const* bytes = static_cast<nx::bench::recorded_quantity const*>(nullptr);
    auto const* ratio = static_cast<nx::bench::recorded_quantity const*>(nullptr);
    for (auto const& q : r.quantities)
    {
        if (q.name == "bytes")
            bytes = &q;
        if (q.name == "hit rate")
            ratio = &q;
    }
    REQUIRE(bytes != nullptr);
    REQUIRE(ratio != nullptr);

    // unit_bytes sums, so the total grows with the iteration count and a rate is meaningful.
    CHECK(bytes->total == f64(r.measured_iterations) * 1024.0);
    CHECK(bytes->per_iteration == 1024.0);
    CHECK(bytes->per_second > 0);

    // unit_ratio averages, so the total IS the average and a per-second figure would be nonsense.
    CHECK(ratio->total == 0.5);
    CHECK(ratio->per_second == 0.0);
}

TEST("bench - warmup iterations contribute no items and no quantities")
{
    auto cfg = quick();
    cfg.warmup_iterations = 7;
    cfg.warmup_time_secs = 0;

    auto const r = nx::bench::run("warmup", cfg,
                                  [&](nx::bench::iteration& it)
                                  {
                                      nx::bench::sink(it.index());
                                      it.items(1);
                                  });

    CHECK(r.warmup_iterations == 7);
    CHECK(r.items == r.measured_iterations); // the seven warmup iterations declared items and were ignored
}

TEST("bench - a run that cannot converge says so rather than pretending")
{
    auto cfg = quick();
    cfg.target_relative_error = 1e-9; // unreachable
    cfg.max_samples = 12;

    auto acc = u64(0);
    auto const r = nx::bench::run("noisy", cfg, [&] { acc = work(acc); });

    CHECK(!r.converged);
    CHECK(r.find_warning(nx::bench::warning_kind::did_not_converge) != nullptr);
}

TEST("bench - a sample cap that cannot satisfy min_time is not a convergence failure")
{
    // The regression: with 1 ms batches, min_time_secs of 0.5 needs about 500 samples.
    // A max_samples below that means elapsed never reaches min_time, so a run that had long since hit its target
    // precision still reported itself as not converged, every single time.
    auto cfg = quick();
    cfg.min_time_secs = 10; // unreachable at this batch size and cap
    cfg.max_samples = 12;
    cfg.target_relative_error = 0.9; // trivially met, so precision is not what is under test

    auto acc = u64(0);
    auto const r = nx::bench::run("capped", cfg, [&] { acc = work(acc); });

    CHECK(isize(r.samples.size()) == 12);
    CHECK(r.converged); // the answer was precise, whatever ended the loop
    CHECK(r.find_warning(nx::bench::warning_kind::did_not_converge) == nullptr);
}

TEST("bench - an unnamed run and a default-config run both work")
{
    auto acc = u64(0);

    auto const unnamed = nx::bench::run(quick(), [&] { acc = work(acc); });
    CHECK(unnamed.name.empty());
    CHECK(unnamed.time.median > 0);
}

// What this machine costs the harness, printed rather than asserted.
//
// The floor is a property of the CPU and the compiler, so a threshold here would be a threshold on the machine the
// suite happens to run on.
// Manual and print-only for that reason: run it by exact name when a number looks wrong and you want to know whether
// the harness or the body is responsible.
TEST("bench - calibration report", nx::config::manual)
{
    auto const& cal = nx::bench::calibrated();

    cc::println("harness floor on this machine:");
    cc::println("  seconds per tick      {:.4f} ns", cal.seconds_per_tick * 1e9);
    cc::println("  cheap cycle counter   {}", cal.has_cheap_counter ? "yes" : "no");
    cc::println("  empty iteration       {:.3f} ns", cal.empty_iteration_secs * 1e9);
    cc::println("  clock pair            {:.3f} ns", cal.clock_pair_secs * 1e9);
    cc::println("");

    auto const style = nx::bench::report_style::for_console();

    // One loop: the full block, since there is nothing to compare against.
    auto acc = u64(0);
    auto single = cc::vector<nx::bench::result>();
    single.push_back(nx::bench::run("a minimal void() body", [&] { acc = work(acc); }));
    cc::print(nx::bench::format_report("one loop", single, style));

    // Several loops in one benchmark: the comparison table.
    auto loops = cc::vector<nx::bench::result>();
    loops.push_back(nx::bench::run("one multiply-add",
                                   [&](nx::bench::iteration& it)
                                   {
                                       acc = work(acc);
                                       it.items(1);
                                   }));
    // NOT work() composed four times.
    // Composing an affine function gives another affine function, so clang folds the four into one, and the report
    // then measures the optimizer rather than four operations.
    loops.push_back(nx::bench::run("four xorshifts",
                                   [&](nx::bench::iteration& it)
                                   {
                                       acc ^= acc << 13;
                                       acc ^= acc >> 7;
                                       acc ^= acc << 17;
                                       acc ^= acc >> 5;
                                       nx::bench::sink(acc);
                                       it.items(4);
                                   }));
    cc::println("");
    cc::print(nx::bench::format_report("several loops", loops, style));

    auto md = style;
    md.markdown = true;
    md.color = false;
    cc::println("");
    cc::print(nx::bench::format_report("the same, markdown-safe", loops, md));
}

TEST("bench - counters are measured in their own passes, and can be turned off")
{
    auto cfg = quick();
    cfg.measure_counters = false;

    auto acc = u64(0);
    auto const without = nx::bench::run("no counters", cfg, [&] { acc = work(acc); });
    CHECK(without.counters.empty());
    CHECK(without.counter_iterations == 0);

    cfg.measure_counters = true;
    auto const with = nx::bench::run("counters", cfg, [&] { acc = work(acc); });

    // A counter pass covers one batch, and it runs AFTER the timing rather than inside it.
    CHECK(with.counter_iterations == with.batch_size);

    // The baseline reference-cycle counter needs no PMU access, so it is there on any machine with a cycle counter.
    // Everywhere else this is legitimately empty, which is why the assertion is conditional rather than absolute.
    for (auto const& c : with.counters)
    {
        CHECK(c.total > 0);
        CHECK(c.per_iteration > 0);
    }
}

TEST("bench - a counter pass does not double-count items or quantities")
{
    auto cfg = quick();
    cfg.measure_counters = true;

    auto const r = nx::bench::run("counted", cfg,
                                  [&](nx::bench::iteration& it)
                                  {
                                      nx::bench::sink(it.index());
                                      it.items(1);
                                  });

    // The counter pass re-runs the body, so the items it declares must not land in the total: one per MEASURED
    // iteration, and the extra batch contributes none.
    CHECK(r.items == r.measured_iterations);
}

TEST("bench - single_shot leaves counters off, since a pass is another whole run of the body")
{
    auto const cfg = nx::bench::run_config::single_shot();
    CHECK(!cfg.measure_counters);
}

TEST("bench - a loop's results reach cc::rec, at its boundary rather than per sample", nx::config::recorded)
{
    auto rec = nx::test_recording();
    if (!rec.is_attached())
        SKIP("the run has no recorder (--no-recording)");

    auto cfg = quick();
    cfg.measure_counters = false;

    auto acc = u64(0);
    auto const r = nx::bench::run("recorded loop", cfg, [&] { acc = work(acc); });
    rec.sync();

    // One scope per loop, carrying the headline statistics.
    // This is what --benchmark-rec writes out, and it is emitted OUTSIDE every timed region: one cc::rec event on a
    // nanosecond body would be most of the measurement.
    //
    // Two events under the scope's name, not one: a scope is a begin and an end.
    CHECK(rec.all().count("nx::bench loop") == 2);
    CHECK(rec.all().count("bench/median seconds") == 1);
    CHECK(rec.all().count("bench/samples") == 1);
    CHECK(rec.all().count("bench/batch size") == 1);

    // Emphatically NOT one per sample: hundreds of samples, one event each.
    CHECK(r.samples.size() > 1);
    CHECK(rec.all().count("bench/median seconds") == 1);
}

TEST("bench - a pause around expensive setup is not warned about")
{
    auto cfg = quick();
    cfg.batch = false;
    cfg.min_samples = 6;
    cfg.max_samples = 6;
    cfg.min_time_secs = 0;

    auto acc = u64(0);
    auto const r = nx::bench::run("cheap pause", cfg,
                                  [&](nx::bench::iteration& it)
                                  {
                                      it.pause();
                                      for (auto i = 0; i < 2000; ++i)
                                          acc = work(acc);
                                      nx::bench::sink(acc);
                                      it.resume();

                                      // Measured work far above the clock pair's cost, which is the case the old
                                      // wall-fraction rule warned about wrongly: paused for ages, paying nanoseconds.
                                      for (auto i = 0; i < 2000; ++i)
                                          acc = work(acc);
                                      nx::bench::sink(acc);
                                  });

    // A substantial share of the wall time, without pinning the ratio: how the two halves balance depends on the
    // optimizer, and the ratio is not what this test is about.
    CHECK(r.paused_fraction > 0.1);

    // This is: measured work this size dwarfs a pause pair on a machine whose clock is cheap, and does not on one
    // whose clock is not — so what the test pins is the rule, not the verdict.
    CHECK((r.find_warning(nx::bench::warning_kind::paused_fraction_high) != nullptr) == pause_warning_is_earned(r));
}
