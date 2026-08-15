#pragma once

#include <nexus/fwd.hh>

namespace nx::config
{
struct cfg;
enum class scheduler_mode;
} // namespace nx::config

// Which selection bucket a test belongs to; a test lives in exactly one.
// An automatic sweep selects a single bucket — normal by default, manual via --manual, guide_benchmark via --guide-benchmarks.
// An exact (non-substring) filter naming a test can also pull it in from another bucket, but only when no bucket flag was given.
// The set is intentionally extensible.
enum class nx::config::test_bucket
{
    normal,
    manual,
    guide_benchmark,
};

// How a test's body is driven.
// Tests sharing a mode form one graph and run as one phase; the phases run one after another, because schedulers do not nest.
// Orthogonal to buckets and to exclusion: it says *where* a test runs, not *whether* or *alongside what*.
enum class nx::config::scheduler_mode
{
    shared,   // the run's own scheduler, bounded by --jobs
    own_pool, // a private pool of `scheduler_threads` workers, shared by every test asking for that same count
    none,     // no scheduler bound at all, bodies driven directly on the calling thread, in schedule order
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

    // Tags whose holders must never run at the same time, compared by content.
    // `exclusion_tag_count` counts what was ASKED for, so it may exceed the array and execute_tests reports the overflow rather than dropping it silently.
    char const* exclusion_tags[max_exclusion_tags] = {};
    int exclusion_tag_count = 0;
    bool exclusive_global = false; // excludes every other test, not just fellow tag holders
};

namespace nx::config
{

// Orthogonal to buckets: a disabled test is skipped by a sweep of any bucket.
// It runs only when named exactly, or under a bulk "run disabled too" request.
constexpr struct
{
    void apply(cfg& result) const { result.enabled = false; }
} disabled;

// A manual test never runs as part of an automatic sweep, not by default and not under a "run disabled too" bulk request either.
// It runs when a filter names it exactly, or when the runner is put in manual mode via --manual.
// Intended for tests that open windows, or are otherwise incompatible with unattended execution.
constexpr struct
{
    void apply(cfg& result) const { result.bucket = test_bucket::manual; }
} manual;

// A guide benchmark records performance metrics via nx::guide, and is swept only via --guide-benchmarks.
// Naming it exactly also runs it, as long as no bucket flag was given; like a manual test it otherwise stays out of automatic runs.
// GUIDE_BENCHMARK in test.hh is the macro.
constexpr struct
{
    void apply(cfg& result) const { result.bucket = test_bucket::guide_benchmark; }
} guide_benchmark;

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

// Run this test with NO scheduler bound: its body is driven directly, in schedule order, alongside the other tests asking for the same.
// Required by a test that stands up its own cc scheduler, or that nests an nx::execute_tests run — neither may sit under the run's own.
constexpr struct
{
    void apply(cfg& result) const { result.scheduler = scheduler_mode::none; }
} no_scheduler;

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
