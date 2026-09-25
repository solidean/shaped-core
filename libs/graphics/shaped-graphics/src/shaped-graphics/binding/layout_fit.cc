#include <clean-core/container/small_vector.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/binding_group_layout.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/binding/layout_fit.hh>
#include <shaped-graphics/binding/pipeline_layout.hh>

using namespace cc::primitive_defines;

namespace
{
[[nodiscard]] bool is_inline_block(sg::binding const& reflected, cc::optional<sg::binding> const& inline_constants)
{
    if (!inline_constants.has_value() || reflected.type != sg::binding_type::uniform_buffer)
        return false;
    auto const& block = inline_constants.value();
    // A push-constant block lives in no set and no space, whatever it is called.
    if (reflected.name == block.name || (!reflected.group_index.has_value() && !reflected.space.has_value()))
        return true;
    return reflected.space.has_value() && reflected.space == block.space && reflected.index == block.index;
}
} // namespace

cc::string sg::describe_layout_misfit(cc::string_view entry,
                                      cc::span<binding const> reflected,
                                      cc::span<cc::span<binding const> const> groups,
                                      cc::optional<binding> const& inline_constants)
{
    auto out = cc::string();
    for (auto const& r : reflected)
    {
        if (is_inline_block(r, inline_constants))
            continue;

        binding const* declared = nullptr;
        auto slot = u32(0);
        for (auto i = isize(0); i < groups.size() && declared == nullptr; ++i)
            for (auto const& d : groups[i])
                if (d.name == r.name)
                {
                    declared = &d;
                    slot = u32(i);
                    break;
                }

        if (declared == nullptr)
        {
            if (r.type != binding_type::sampler)
                out += cc::format("{}: reflects '{}', which no group of the layout declares\n", entry, r.name);
            continue;
        }

        if (r.group_index.has_value() && r.group_index.value() != slot)
            out += cc::format("{}: '{}' is in set {}, and the layout has its group at slot {}\n", entry, r.name,
                              r.group_index.value(), slot);
        auto const space = declared->space.value_or(slot);
        if (r.space.has_value() && r.space.value() != space)
            out += cc::format("{}: '{}' is in space {}, and the layout places it in space {}\n", entry, r.name,
                              r.space.value(), space);
        if (r.index != declared->index || r.count != declared->count || !is_same_kind(r, *declared))
            out += cc::format("{}: '{}' reflects as index {}, count {}, kind {}, access {}, and the layout declares "
                              "{}, "
                              "{}, {}, {}\n",
                              entry, r.name, r.index, r.count, int(r.type), int(r.access), declared->index,
                              declared->count, int(declared->type), int(declared->access));
    }
    return out;
}

cc::string sg::describe_layout_misfit(compiled_shader const& shader, pipeline_layout const& layout)
{
    auto groups = cc::small_vector<cc::span<binding const>, max_binding_groups>();
    for (auto const& group : layout.groups())
        groups.push_back(group != nullptr ? group->bindings() : cc::span<binding const>());
    return describe_layout_misfit(shader.entry_point, shader.bindings, groups, layout.inline_constants());
}
