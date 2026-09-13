#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/binding/impl/binding_conflicts.hh>

using namespace cc::primitive_defines;

namespace
{
/// HLSL numbers registers per class, so `t0` and `b0` are different addresses that both reflect as index 0.
/// SPIR-V numbers a whole set at once, so there the class is not part of an address and must not be compared.
/// A binding carries whichever its language reflects — see binding.hh — which is what tells the two cases apart.
[[nodiscard]] u32 register_class_of(sg::binding const& b)
{
    if (b.group_index.has_value())
        return 0; // a set-numbered binding: one address space for every kind

    switch (b.type)
    {
    case sg::binding_type::uniform_buffer:
        return 1; // b
    case sg::binding_type::sampler:
        return 2; // s
    case sg::binding_type::readwrite_structured_buffer:
    case sg::binding_type::readwrite_raw_buffer:
    case sg::binding_type::readwrite_texture:
        return 3; // u
    case sg::binding_type::readonly_structured_buffer:
    case sg::binding_type::readonly_raw_buffer:
    case sg::binding_type::readonly_texture:
    case sg::binding_type::acceleration_structure:
        return 4; // t
    }
    return 4;
}

struct address
{
    cc::optional<u32> group;
    cc::optional<u32> space;
    u32 register_class = 0;
    u32 index = 0;

    [[nodiscard]] bool operator==(address const& rhs) const
    {
        return group == rhs.group && space == rhs.space && register_class == rhs.register_class && index == rhs.index;
    }
};

[[nodiscard]] address address_of(sg::binding const& b)
{
    return {.group = b.group_index, .space = b.space, .register_class = register_class_of(b), .index = b.index};
}

[[nodiscard]] cc::string describe(address const& a)
{
    auto const group = a.group.has_value() ? cc::format("group {}", a.group.value()) : cc::string("no group");
    auto const space = a.space.has_value() ? cc::format(", space {}", a.space.value()) : cc::string();
    return cc::format("({}{}, index {})", group, space, a.index);
}

struct seen_binding
{
    sg::binding const* binding = nullptr;
    address addr;
    cc::string_view entry_point;
};
} // namespace

cc::optional<cc::string> sg::impl::find_binding_conflict(cc::span<compiled_shader const* const> shaders)
{
    // Linear over a handful of bindings across a handful of stages, on a path that already builds a pipeline.
    cc::vector<seen_binding> seen;

    for (auto const* const shader : shaders)
    {
        if (shader == nullptr)
            continue;

        for (auto const& b : shader->bindings)
        {
            auto const addr = address_of(b);

            for (auto const& other : seen)
            {
                bool const same_address = other.addr == addr;
                bool const same_name = other.binding->name == b.name;

                if (same_address && !same_name)
                    return cc::format(
                        "two stages disagree about what lives at {}: '{}' in '{}' and '{}' in '{}'. Shaders sharing a "
                        "group must declare it in one shared header, so both see the same numbering.",
                        describe(addr), other.binding->name, other.entry_point, b.name, shader->entry_point);

                if (same_address && same_name && other.binding->type != b.type)
                    return cc::format("two stages declare '{}' at {} with different kinds, in '{}' and '{}'", b.name,
                                      describe(addr), other.entry_point, shader->entry_point);

                if (same_name && !same_address)
                    return cc::format(
                        "'{}' sits at {} in '{}' but at {} in '{}'. A binding group resolves a name, so one name "
                        "cannot be bound at two addresses.",
                        b.name, describe(other.addr), other.entry_point, describe(addr), shader->entry_point);
            }

            seen.push_back({.binding = &b, .addr = addr, .entry_point = shader->entry_point});
        }
    }

    return {};
}
