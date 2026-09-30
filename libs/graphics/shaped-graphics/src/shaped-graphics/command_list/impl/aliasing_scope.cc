#include <clean-core/common/assertf.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/command_list/impl/aliasing_scope.hh>

namespace sg::impl
{
namespace
{
template <class Use>
cc::string where_of(Use const& u)
{
    if (u.group >= 0)
        return cc::format("binding '{}' of group {}", u.binding, u.group);
    if (u.vertex_slot >= 0)
        return cc::format("vertex buffer slot {}", u.vertex_slot);
    return cc::string("the index buffer");
}
} // namespace

void aliasing_scope::add(cc::span<binding_group const* const> groups,
                         cc::span<raw_buffer const* const> vertex_buffers,
                         raw_buffer const* index_buffer)
{
    for (auto g = isize(0); g < groups.size(); ++g)
        if (groups[g] != nullptr)
            for (auto const& u : buffer_uses_of(*groups[g]))
                add({.buffer = u.buffer, .writes = u.writes, .group = int(g), .binding = u.binding});
    for (auto s = isize(0); s < vertex_buffers.size(); ++s)
        if (vertex_buffers[s] != nullptr)
            add({.buffer = vertex_buffers[s], .vertex_slot = int(s)});
    if (index_buffer != nullptr)
        add({.buffer = index_buffer});
}

void aliasing_scope::add(use const& u)
{
    auto seen = false;
    for (auto const& other : _uses)
    {
        if (other.buffer != u.buffer)
            continue;
        auto const& writer = u.writes ? u : other;
        auto const& reader = u.writes ? other : u;
        CC_ASSERTF_ALWAYS(other.writes == u.writes,
                          "{} writes a buffer that {} reads, in one dispatch or draw; WebGPU refuses that "
                          "whatever the ranges, so sg refuses it on every backend: copy it, or split the work",
                          where_of(writer), where_of(reader));
        seen = true;
    }
    if (!seen)
        _uses.push_back(u);
}
} // namespace sg::impl
