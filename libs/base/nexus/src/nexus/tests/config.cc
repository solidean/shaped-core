#include "config.hh"

void nx::impl::apply_config_item(config::cfg& result, config::cfg const& rhs)
{
    result.enabled &= rhs.enabled;

    if (rhs.seed != 0)
        result.seed = rhs.seed;
}
