#include <clean-core/common/assertf.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/binding/binding.hh>

namespace sg
{
void merge_bindings(cc::vector<binding>& into, cc::span<binding const> from)
{
    for (auto const& b : from)
    {
        auto seen = false;
        for (auto const& e : into)
            if (e.name == b.name)
            {
                seen = true;
                break;
            }

        if (!seen)
            into.push_back(b);
    }
}

cc::vector<binding> merge_bindings(cc::span<cc::span<binding const> const> stages)
{
    auto merged = cc::vector<binding>();
    for (auto const& s : stages)
        merge_bindings(merged, s);
    return merged;
}

cc::vector<binding> split_off_sampler_bindings(cc::vector<binding>& bindings)
{
    auto samplers = cc::vector<binding>();

    auto kept = isize(0);
    for (auto i = isize(0); i < bindings.size(); ++i)
    {
        if (is_sampler(bindings[i].type))
        {
            samplers.push_back(cc::move(bindings[i]));
            continue;
        }

        if (kept != i)
            bindings[kept] = cc::move(bindings[i]);
        ++kept;
    }
    bindings.resize_down_to(kept);

    return samplers;
}

cc::optional<u32> group_index_of(cc::span<binding const> bindings)
{
    auto found = cc::optional<u32>();
    for (auto const& b : bindings)
    {
        if (!b.group_index.has_value())
            continue;

        auto const declared = b.group_index.value();
        auto const agreed = found.value_or(declared);
        CC_ASSERTF(agreed == declared,
                   "bindings of one group layout disagree about their group index ({} vs {}, at '{}')", agreed,
                   declared, b.name);
        found = declared;
    }
    return found;
}
} // namespace sg
