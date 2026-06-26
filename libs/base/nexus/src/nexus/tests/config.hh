#pragma once

namespace nx::config
{
// Which selection bucket a test belongs to. A test lives in exactly one bucket; an automatic sweep selects a
// single bucket (normal by default, manual via --manual, guide_benchmark via --guide-benchmarks). Naming a
// test by an exact (non-wildcard) filter runs it regardless of its bucket. The set is intentionally extensible.
enum class test_bucket
{
    normal,
    manual,
    guide_benchmark,
};

struct cfg
{
    bool enabled = true;
    test_bucket bucket = test_bucket::normal;
    int seed = 0;
};

// Orthogonal to buckets: a disabled test is skipped by a sweep of any bucket and only runs when explicitly
// named (or via a bulk "run disabled too" request).
constexpr struct
{
    void apply(cfg& result) const { result.enabled = false; }
} disabled;

// A manual test never runs as part of an automatic sweep — not by default, and not via a "run disabled too"
// bulk request either. It runs only when explicitly targeted (a non-wildcard filter that names it) or when
// the runner is put in manual mode via --manual. Intended for tests that open windows or are otherwise
// incompatible with unattended execution.
constexpr struct
{
    void apply(cfg& result) const { result.bucket = test_bucket::manual; }
} manual;

// A guide benchmark records performance metrics via nx::guide and is swept only via --guide-benchmarks (or
// named explicitly). Like manual tests it stays out of automatic runs. See GUIDE_BENCHMARK in test.hh.
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

// merge logic
// NOTE: defaults in cfg must not override values in result
void apply_config_item(config::cfg& result, config::cfg const& rhs);

// for the struct -> void apply(cfg&) pattern
void apply_config_item(config::cfg& result, auto const& config)
    requires requires { config.apply(result); }
{
    config.apply(result);
}

// given a list of configs or configure objects, create a single test config from that
config::cfg merge_config(auto&&... items)
{
    config::cfg result;
    (impl::apply_config_item(result, items), ...);
    return result;
}

} // namespace nx::impl
