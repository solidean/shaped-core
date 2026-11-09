#include "metal_binding_group.hh"

#include <shaped-graphics/backends/metal/metal_acceleration_structure.hh>
#include <shaped-graphics/backends/metal/metal_binding_layout.hh>
#include <shaped-graphics/backends/metal/metal_buffer.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>

namespace sg::backend::metal
{
metal_binding_group::metal_binding_group(metal_context& ctx,
                                         metal_binding_group_layout_handle layout,
                                         MTL::Buffer* arguments,
                                         cc::vector<bound_buffer> bound_buffers,
                                         cc::vector<bound_texture> bound_textures,
                                         cc::vector<sg::tlas_handle> bound_tlases,
                                         cc::vector<array_binding> array_bindings,
                                         isize hit_offsets_at,
                                         cc::vector<u32> acceleration_roots)
  : _ctx(ctx),
    _layout(cc::move(layout)),
    _arguments(arguments),
    _hit_offsets_at(hit_offsets_at),
    _bound_buffers(cc::move(bound_buffers)),
    _bound_textures(cc::move(bound_textures)),
    _bound_tlases(cc::move(bound_tlases)),
    _array_bindings(cc::move(array_bindings)),
    _acceleration_roots(cc::move(acceleration_roots))
{
}

cc::result<metal_binding_group::created_arguments> metal_binding_group::create_arguments(metal_context& ctx,
                                                                                         cc::string_view label,
                                                                                         argument_contents const& contents)
{
    auto const slot_count = contents.unbounded_at >= 0 ? contents.unbounded_at + 1 : contents.slots.size();
    auto const elements = contents.unbounded_at >= 0 ? contents.slots.size() - contents.unbounded_at : isize(0);
    auto const aligned = [](isize n) { return (n + 7) / 8 * 8; };

    auto const lengths_at = slot_count * isize(sizeof(u64));
    auto const mirror_at = aligned(lengths_at + slot_count * isize(sizeof(u32)));
    auto const table_at = mirror_at + contents.mirror.size() * isize(sizeof(u64));
    auto const end = contents.unbounded_at >= 0
                       ? table_at + 2 * isize(sizeof(u32)) + elements * isize(sizeof(u64) + sizeof(u32))
                       : table_at;
    // an empty layout still gets a buffer: an argument table binds an address, and a null one is not a legal binding
    auto* const buffer
        = ctx.device()->newBuffer(NS::UInteger(cc::max(isize(sizeof(u64)), end)),
                                  MTL::ResourceStorageModeShared | MTL::ResourceHazardTrackingModeUntracked);
    if (buffer == nullptr)
        return cc::error(cc::format("{}: the metal device refused the argument buffer", label));
    buffer->setLabel(ns_string(label));

    auto* const bytes = static_cast<byte*>(buffer->contents());
    cc::memcpy(bytes, contents.slots.data(), size_t(slot_count * isize(sizeof(u64))));
    cc::memcpy(bytes + lengths_at, contents.lengths.data(), size_t(slot_count * isize(sizeof(u32))));
    if (!contents.mirror.empty())
        cc::memcpy(bytes + mirror_at, contents.mirror.data(), size_t(contents.mirror.size() * isize(sizeof(u64))));
    if (contents.unbounded_at >= 0)
    {
        auto const table_address = u64(buffer->gpuAddress()) + u64(table_at);
        cc::memcpy(bytes + contents.unbounded_at * isize(sizeof(u64)), &table_address, sizeof(u64));
        auto const no_length = u32(0);
        cc::memcpy(bytes + lengths_at + contents.unbounded_at * isize(sizeof(u32)), &no_length, sizeof(u32));
        u32 const header[2] = {u32(elements), 0u};
        cc::memcpy(bytes + table_at, header, sizeof(header));
        auto const entries_at = table_at + 2 * isize(sizeof(u32));
        cc::memcpy(bytes + entries_at, contents.slots.data() + contents.unbounded_at,
                   size_t(elements * isize(sizeof(u64))));
        cc::memcpy(bytes + entries_at + elements * isize(sizeof(u64)), contents.lengths.data() + contents.unbounded_at,
                   size_t(elements * isize(sizeof(u32))));
    }
    ctx.residency().add(buffer);
    return created_arguments{.buffer = buffer, .hit_offsets_at = contents.mirror.empty() ? isize(-1) : mirror_at};
}

u32 metal_binding_group::byte_length_of(sg::raw_buffer_view const& view)
{
    return u32(view.shape == sg::view_shape::structured ? view.element_count * view.stride_in_bytes : view.size_in_bytes);
}

cc::vector<u64> metal_binding_group::hit_offsets_mirror(cc::span<sg::tlas_handle const> tlas_at_slot)
{
    auto mirror = cc::vector<u64>::create_filled(tlas_at_slot.size(), u64(0));
    auto holds_tlas = false;
    for (auto slot = isize(0); slot < tlas_at_slot.size(); ++slot)
    {
        auto const* const tlas = static_cast<metal_tlas const*>(tlas_at_slot[slot].get());
        if (tlas == nullptr)
            continue;
        holds_tlas = true;
        if (tlas->hit_group_offsets() != nullptr)
            mirror[slot] = static_cast<metal_buffer const&>(*tlas->hit_group_offsets()).gpu_address();
    }
    if (!holds_tlas)
        mirror.clear();
    return mirror;
}

metal_binding_group::~metal_binding_group()
{
    if (_arguments == nullptr)
        return;
    auto* const buffer = _arguments;
    _arguments = nullptr;
    _ctx.residency().remove(buffer);
    _ctx.epochs().defer([buffer] { buffer->release(); });
}
} // namespace sg::backend::metal
