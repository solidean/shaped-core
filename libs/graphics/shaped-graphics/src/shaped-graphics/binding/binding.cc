#include <clean-core/common/assertf.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/binding/binding.hh>

namespace sg
{
void apply_stage_visibility(cc::span<binding> bindings, shader_stage stage)
{
    for (auto& b : bindings)
        b.visibility.set(stage);
}

void merge_bindings(cc::vector<binding>& into, cc::span<binding const> from)
{
    for (auto const& b : from)
    {
        auto* existing = static_cast<binding*>(nullptr);
        for (auto& e : into)
            if (e.name == b.name)
            {
                existing = &e;
                break;
            }

        if (existing == nullptr)
        {
            into.push_back(b);
            continue;
        }

        // First-seen wins for every field except visibility, which is the one thing the merge exists to accumulate:
        // one compiled_shader is one stage, so a binding declared by two stages arrives here twice, once per bit.
        existing->visibility |= b.visibility;
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
