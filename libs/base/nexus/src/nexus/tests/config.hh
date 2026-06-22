#pragma once

namespace nx::config
{
struct cfg
{
    bool enabled = true;
    int seed = 0;
};

constexpr struct
{
    void apply(cfg& result) const { result.enabled = false; }
} disabled;

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