#pragma once

#include <nexus/fwd.hh>

namespace nx::config
{
struct cfg;
enum class scheduler_mode;
enum class ambient_mode;
} // namespace nx::config

// Which selection bucket a test belongs to; a test lives in exactly one.
// An automatic sweep selects a single bucket — normal by default, manual via --manual, pgo_benchmark via --pgo-benchmarks, benchmark via --benchmarks, example via --examples.
// An exact (non-substring) filter naming a test can also pull it in from another bucket, but only when no bucket flag was given.
// The set is intentionally extensible.
enum class nx::config::test_bucket
{
    normal,
    manual,
    pgo_benchmark,
    benchmark,
    example,
};

// WHERE a test's body runs.
// Tests sharing a mode form one graph and run as one phase; the phases run one after another, because schedulers do not nest.
// Orthogonal to buckets and to exclusion: it says *where* a test runs, not *whether* or *alongside what*.
//
// Distinct from ambient_mode, which says which scheduler the test's own async work belongs to.
// A body driven directly still has one — see nx::no_scheduler for the test that wants none.
enum class nx::config::scheduler_mode
{
    shared,   // the run's own scheduler, bounded by --jobs
    own_pool, // a private pool of `scheduler_threads` workers, shared by every test asking for that same count
    none,     // bodies driven directly on the calling thread, in schedule order
};

// WHICH scheduler the async system uses inside a test — the one cc::install_default_async_scheduler installs.
// Every async needs one, so a run provides it; a test only names this to get something other than the default.
enum class nx::config::ambient_mode
{
    multi_threaded, // a pool; the phase's own where the bodies run on it, otherwise one stood up for the phase
    single_threaded, // a cc::singlethreaded_scheduler bound to the body's thread, so every graph runs inline and in order
    none,            // nothing installed and nothing bound: the test stands up its own, or asserts on touching an async
};

namespace nx::config
{
// The most exclusion tags one test may carry.
// A raw array of literals rather than a container: cfg stays trivially copyable, and config.hh stays the light header every test TU includes.
inline constexpr int max_exclusion_tags = 4;
} // namespace nx::config

struct nx::config::cfg
{
    bool enabled = true;
    test_bucket bucket = test_bucket::normal;
    int seed = 0;

    scheduler_mode scheduler = scheduler_mode::shared;
    int scheduler_threads = 0; // only read for scheduler_mode::own_pool
    ambient_mode ambient = ambient_mode::multi_threaded;

    bool main_thread = false; // the body must run on the process main thread; orthogonal to `scheduler`

    // A command line for this test, reachable from its body through nx::test_args().
    // A `char const*` for the same reason exclusion_tags is an array of them: cfg stays trivially copyable,
    // and config.hh stays the light header every test TU includes.
    // nullptr means none was declared, which is distinct from `""` — an explicitly empty line.
    char const* test_args = nullptr;

    // Tags whose holders must never run at the same time, compared by content.
    // `exclusion_tag_count` counts what was ASKED for, so it may exceed the array and execute_tests reports the overflow rather than dropping it silently.
    char const* exclusion_tags[max_exclusion_tags] = {};
    int exclusion_tag_count = 0;
    bool exclusive_global = false; // excludes every other test, not just fellow tag holders

    // Bucket this test's cc::rec events so nx::test_recording() can read them back.
    // OFF by default, because attribution is paid per test whether or not the test ever asks — see
    // libs/base/nexus/docs/recording.md.
    bool recorded = false;

    // This test drives cc::rec::initialize / shutdown itself, so the run hands its own recorder over for the duration.
    // Requires an untagged `exclusive()` and forbids `recorded`; the schedule asserts on both rather than trusting the
    // order the config objects were spelled in.
    bool owns_recorder = false;
};

namespace nx::config
{

// A command line this test receives, as it would be typed.
//
// Tokenized once when the schedule is built, by the same rules a response file uses, and reachable from the
// body through nx::test_args().
// The point is an EXAMPLE that demonstrates something without the reader having to type anything:
//
//     EXAMPLE("mytool/build", nx::config::args("--jobs 8 --verbose"))
//
// `--test-args` on the command line REPLACES this rather than adding to it: merging two argument lines is a
// semantic swamp, and replacement is the one rule that stays predictable.
constexpr auto args(char const* command_line)
{
    struct item
    {
        char const* value;
        void apply(cfg& c) const { c.test_args = value; }
    };

    return item{command_line};
}

// Orthogonal to buckets: a disabled test is skipped by a sweep of any bucket.
// It runs only when named exactly, or under a bulk "run disabled too" request.
constexpr struct
{
    void apply(cfg& result) const { result.enabled = false; }
} disabled;

// Bucket this test's cc::rec events, so nx::test_recording() can read them back and a failure keeps them.
//
// Opt-in, because attribution is per TEST rather than per event: a trace link, plus an ambient delta wherever the work
// moves between contexts.
// A test that never asks would pay all of it for an answer nobody reads, and most tests never ask.
constexpr struct
{
    void apply(cfg& result) const { result.recorded = true; }
} recorded;

// This test owns the cc::rec singleton: the run shuts its recorder down before the body and re-initializes after.
//
// **Requires an untagged `exclusive()`**, since a recorder torn down on one thread is torn down for every thread —
// a tagged exclusive only holds off fellow tag holders, which is not enough.
// It also forbids `recorded`: there is no run recorder left to bucket into.
// Both are asserted when the schedule is built, so neither depends on the order these are spelled in.
constexpr struct
{
    void apply(cfg& result) const { result.owns_recorder = true; }
} owns_recorder;

// A manual test never runs as part of an automatic sweep, not by default and not under a "run disabled too" bulk request either.
// It runs when a filter names it exactly, or when the runner is put in manual mode via --manual.
// Intended for tests that open windows, or are otherwise incompatible with unattended execution.
constexpr struct
{
    void apply(cfg& result) const { result.bucket = test_bucket::manual; }
} manual;

// A PGO benchmark records performance metrics via nx::pgo, and is swept only via --pgo-benchmarks.
// Naming it exactly also runs it, as long as no bucket flag was given; like a manual test it otherwise stays out of automatic runs.
// PGO_BENCHMARK in test.hh is the macro.
constexpr struct
{
    void apply(cfg& result) const { result.bucket = test_bucket::pgo_benchmark; }
} pgo_benchmark;

// A benchmark measures something with nx::bench::run, and is swept only via --benchmarks.
//
// It also runs ALONE: `exclusive_global` is set here rather than left to the author, because two tests sharing a
// machine share its caches, its cores and its memory bandwidth, and a timing taken while another test runs is a timing
// of the pair.
// BENCHMARK in test.hh is the macro, and it adds `main_thread` on top of this.
constexpr struct
{
    void apply(cfg& result) const
    {
        result.bucket = test_bucket::benchmark;
        result.exclusive_global = true;
    }
} benchmark;

// An example demonstrates an API in practice rather than pinning an invariant, and is swept only via --examples.
// Naming it exactly also runs it, as long as no bucket flag was given; like a manual test it otherwise stays out of automatic runs.
// Its body needs no CHECK at all — being free of the testable-only bias is the point — but a CHECK that fails still fails.
// EXAMPLE in test.hh is the macro.
constexpr struct
{
    void apply(cfg& result) const { result.bucket = test_bucket::example; }
} example;

// No two tests holding `tag` run at the same time; with no tag, this test runs alone, concurrent with nothing.
// Expressed as an ordering edge between test nodes rather than a lock, so it is deadlock-free by construction and reproducible: holders run in schedule order.
// Repeat it to hold several tags — a test then waits for the last holder of each.
constexpr auto exclusive(char const* tag = nullptr)
{
    struct excluder
    {
        char const* tag;
        void apply(cfg& result) const
        {
            if (tag == nullptr)
            {
                result.exclusive_global = true;
                return;
            }
            if (result.exclusion_tag_count < max_exclusion_tags)
                result.exclusion_tags[result.exclusion_tag_count] = tag;
            ++result.exclusion_tag_count; // counted even when it does not fit, so the overflow is reportable
        }
    };
    return excluder{tag};
}

// Run this test with NO ambient scheduler at all: none bound to its thread, and none installed as the default.
// Its body is driven directly, in schedule order, alongside the other tests asking for the same.
//
// Required by a test that stands up its own cc scheduler, or that nests an nx::execute_tests run — neither may sit under the run's own.
// Touching an async without one then asserts, which is the point: this is how a test proves it owns that decision.
// Not the way to ask for the main thread: nx::main_thread says that, and says the thing that gets checked.
constexpr struct
{
    void apply(cfg& result) const
    {
        result.scheduler = scheduler_mode::none;
        result.ambient = ambient_mode::none;
    }
} no_scheduler;

// Run this test under a cc::singlethreaded_scheduler: its body is driven directly, and every async it touches runs inline on that same thread.
// For a test whose subject is the ORDER things run in, which a pool is free to change.
constexpr struct
{
    void apply(cfg& result) const
    {
        result.scheduler = scheduler_mode::none;
        result.ambient = ambient_mode::single_threaded;
    }
} singlethreaded;

// Run this test's body on the process MAIN thread — the one nx::run was entered on.
// For a test whose subject asserts on it: sr::window_system does, because SDL does.
// Orthogonal to the scheduler mode: it says WHICH thread, not whether one is bound.
// own_pool and ASYNC_TEST cannot be combined with it and assert, because either could only be honoured by ignoring one of the two asks.
constexpr struct
{
    void apply(cfg& result) const { result.main_thread = true; }
} main_thread;

// Run this test on a private pool of `n` workers, shared with every other test asking for the same count.
// For a test whose own concurrency shape is the thing under test; the default (the run's scheduler) is otherwise the right answer.
constexpr auto own_pool(int n)
{
    struct pooler
    {
        int n;
        void apply(cfg& result) const
        {
            result.scheduler = scheduler_mode::own_pool;
            result.scheduler_threads = n;
        }
    };
    return pooler{n};
}

constexpr auto seed(int value)
{
    struct seeder
    {
        int seed;
        void apply(cfg& result) const { result.seed = seed; }
    };
    return seeder{value};
}

} // namespace nx::config

namespace nx::impl
{

// Folds one config item into the accumulated cfg.
// A default in `rhs` must not override a value already set in `result`.
void apply_config_item(config::cfg& result, config::cfg const& rhs);

// The `struct { void apply(cfg&) }` config-item form, e.g. nx::config::disabled.
void apply_config_item(config::cfg& result, auto const& config)
    requires requires { config.apply(result); }
{
    config.apply(result);
}

// Merges a list of config items into one test config, in the order TEST received them.
config::cfg merge_config(auto&&... items)
{
    config::cfg result;
    (impl::apply_config_item(result, items), ...);
    return result;
}

} // namespace nx::impl
