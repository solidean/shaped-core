#pragma once

#include <clean-core/function/function_ref.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/bench/barriers.hh>
#include <nexus/bench/result.hh>
#include <nexus/bench/run_config.hh>

namespace cc::rec
{
struct unit;
} // namespace cc::rec

namespace nx::bench::impl
{
struct run_state;

/// The engine, behind one type-erased entry point so the sampling logic is compiled once rather than per body type.
///
/// `body` runs `count` iterations and is called once per batch, so the indirect call is amortized over the whole batch
/// and the per-iteration cost is whatever the caller's own loop compiles to.
result run_measured(cc::string_view name, run_config const& cfg, cc::function_ref<void(isize count, iteration& it)> body);

/// Moves the handle to the next iteration.
/// The harness's own loop calls this; a body never does, which is why it is not a member.
void advance(iteration& it, isize index);

/// Points a fresh handle at the run it belongs to, which is the engine's first act.
void bind(iteration& it, run_state* state);
} // namespace nx::bench::impl

/// The handle a body receives, and the only way to say anything about the iteration being measured.
///
/// A body takes one of these only when it needs to: the `void()` form pays nothing for a handle it never touches.
struct nx::bench::iteration
{
    /// Stop the clock.
    /// Everything until `resume` is excluded from the measurement.
    ///
    /// **A last resort, and expensive.**
    /// A pair costs two clock readings, so a body that pauses around three lines of setup and measures four
    /// nanoseconds of work is measuring mostly the clock.
    /// The `paused_fraction_high` warning is what says that happened.
    /// Setup that can live outside the loop belongs outside the loop, and a body that cannot manage that usually wants
    /// the `void(isize)` form instead.
    void pause();
    void resume();

    /// This iteration processed `n` items, which is what turns a per-iteration time into a per-item rate.
    ///
    /// Additive within an iteration, so calling it twice with 3 and 4 declares seven.
    /// A body that never calls it reports no rate at all, which is deliberately distinct from reporting one item per
    /// iteration.
    void items(isize n);

    /// Record a quantity this iteration produced, against what it means.
    ///
    /// The unit decides how the values combine and how they are printed: summing or averaging, binary or decimal
    /// prefixes, and whether more is good news.
    /// `cc::rec::unit_bytes` and friends in clean-core/record/stat.hh are the common ones, and a caller defines its own
    /// next to the code that records it.
    void record(cc::string_view name, cc::rec::unit const& unit, f64 value);

    /// 0-based, and counted across warmup and measurement alike.
    [[nodiscard]] isize index() const { return _index; }

    /// Whether this iteration is a warmup one, whose timings and quantities are all discarded.
    [[nodiscard]] bool is_warmup() const;

private:
    friend struct impl::run_state;
    friend void impl::advance(iteration& it, isize index);
    friend void impl::bind(iteration& it, impl::run_state* state);

    impl::run_state* _state = nullptr;
    isize _index = 0;
};

namespace nx::bench
{
/// Measure `body`, and return everything the run produced.
///
/// **The harness owns the loop**, which is what lets it decide how many times, in what order and under what
/// instrumentation the body runs — re-running it for hardware-counter passes, interleaving it against another body, or
/// repeating a whole selection.
/// A range-`for` could do none of those, which is why this takes a callable.
///
/// **`body` must be repeatable.** It is invoked many times and its observable effect must be the same every time.
///
/// Three signatures are accepted, and the one you wrote is detected:
///
///     void()               the minimal form; the harness's loop is all that stands between iterations
///     void(iteration&)     pause/resume, items(), record()
///     void(isize count)    the body owns the inner loop, and there is one timing boundary per batch
///
/// The last is the escape hatch when the `overhead_significant` warning fires: moving the loop inside the body puts
/// the harness's per-iteration cost to zero by construction.
///
///     nx::bench::run("cc::sort", {.min_time_secs = 0.5}, [&](nx::bench::iteration& it)
///     {
///         it.pause();
///         work = source;
///         it.resume();
///
///         cc::sort(work);
///         nx::bench::sink(work[0]);
///         it.items(source.size());
///     });
///
/// Returning the result is what makes this usable outside a BENCHMARK: in a benchmark run it is collected and reported
/// for you, and anywhere else the caller simply has the numbers.
template <class Body>
result run(cc::string_view name, run_config const& cfg, Body&& body)
{
    // The branch on clobber_each_iteration is hoisted out of every loop below rather than tested per iteration: it is
    // fixed for the whole run, and a per-iteration test would be part of what gets measured.
    if constexpr (requires { body(isize(0)); })
    {
        // The body owns its own loop, so there is nothing for the harness to put between iterations.
        return impl::run_measured(name, cfg, [&](isize count, iteration&) { body(count); });
    }
    else if constexpr (requires(iteration& probe) { body(probe); })
    {
        return impl::run_measured(name, cfg,
                                  [&](isize count, iteration& it)
                                  {
                                      if (cfg.clobber_each_iteration)
                                          for (auto i = isize(0); i < count; ++i)
                                          {
                                              impl::advance(it, i);
                                              body(it);
                                              bench::compiler_barrier();
                                          }
                                      else
                                          for (auto i = isize(0); i < count; ++i)
                                          {
                                              impl::advance(it, i);
                                              body(it);
                                          }
                                  });
    }
    else
    {
        static_assert(requires { body(); }, "a benchmark body must be callable as void(), void(nx::bench::iteration&) "
                                            "or void(isize)");
        return impl::run_measured(name, cfg,
                                  [&](isize count, iteration&)
                                  {
                                      if (cfg.clobber_each_iteration)
                                          for (auto i = isize(0); i < count; ++i)
                                          {
                                              body();
                                              bench::compiler_barrier();
                                          }
                                      else
                                          for (auto i = isize(0); i < count; ++i)
                                              body();
                                  });
    }
}

/// The same, with the default config.
template <class Body>
result run(cc::string_view name, Body&& body)
{
    return bench::run(name, run_config::standard(), cc::forward<Body>(body));
}

/// The same, unnamed — for the benchmark that has exactly one loop and nothing to compare it against.
template <class Body>
result run(run_config const& cfg, Body&& body)
{
    return bench::run(cc::string_view(), cfg, cc::forward<Body>(body));
}
} // namespace nx::bench
