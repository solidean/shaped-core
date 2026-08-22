#include "config.hh"

void nx::impl::apply_config_item(config::cfg& result, config::cfg const& rhs)
{
    result.enabled &= rhs.enabled;

    if (rhs.bucket != config::test_bucket::normal)
        result.bucket = rhs.bucket;

    if (rhs.seed != 0)
        result.seed = rhs.seed;

    if (rhs.scheduler != config::scheduler_mode::shared)
    {
        result.scheduler = rhs.scheduler;
        result.scheduler_threads = rhs.scheduler_threads;
    }

    // Overrides rather than accumulating: two argument lines cannot be merged into one that means anything.
    if (rhs.test_args != nullptr)
        result.test_args = rhs.test_args;

    // A flag, so it accumulates like exclusive_global rather than overriding: a config asking for it wins.
    result.main_thread |= rhs.main_thread;

    // Exclusion accumulates rather than overriding: two config items each naming a tag mean the test holds both.
    result.exclusive_global |= rhs.exclusive_global;
    for (int i = 0; i < rhs.exclusion_tag_count && i < config::max_exclusion_tags; ++i)
    {
        if (result.exclusion_tag_count < config::max_exclusion_tags)
            result.exclusion_tags[result.exclusion_tag_count] = rhs.exclusion_tags[i];
        ++result.exclusion_tag_count;
    }
}
