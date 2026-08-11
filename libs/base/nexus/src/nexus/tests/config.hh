#pragma once

#include <nexus/fwd.hh>

namespace nx::config
{
struct cfg;
} // namespace nx::config

namespace nx::config
{
// Which selection bucket a test belongs to; a test lives in exactly one.
// An automatic sweep selects a single bucket — normal by default, manual via --manual, guide_benchmark via --guide-benchmarks.
// An exact (non-substring) filter naming a test can also pull it in from another bucket, but only when no bucket flag was given.
// The set is intentionally extensible.
enum class test_bucket
{
    normal,
    manual,
    guide_benchmark,
};

} // namespace nx::config

struct nx::config::cfg
{
    bool enabled = true;
    test_bucket bucket = test_bucket::normal;
    int seed = 0;
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
