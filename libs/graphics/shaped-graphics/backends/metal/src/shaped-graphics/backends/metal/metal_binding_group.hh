#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/fwd.hh>

/// Metal implementation of sg::binding_group: one argument buffer, and what it keeps alive.
///
/// **An argument buffer is a flat array of 8-byte slots**, indexed by `binding.index`, which is what `[[id(n)]]`
/// addresses in MSL.
/// A buffer binding holds a GPU address, a texture or sampler binding an `MTLResourceID`.
/// Nothing needs an encoder: the values are written straight into shared memory at creation.
///
/// The group holds every resource it names.
/// A caller may drop its own handle the moment the group is built — an argument buffer is raw addresses, with nothing
/// to keep the target alive — and the group's GPU work still has to find something there.
class sg::backend::metal::metal_binding_group final : public sg::binding_group
{
public:
    metal_binding_group(metal_context& ctx,
                        metal_binding_group_layout_handle layout,
                        MTL::Buffer* arguments,
                        cc::vector<sg::raw_buffer_handle> bound_buffers,
                        cc::vector<sg::raw_texture_handle> bound_textures = {});
    ~metal_binding_group() override;

    /// The address an argument table binds this group at.
    [[nodiscard]] u64 argument_address() const { return _arguments != nullptr ? u64(_arguments->gpuAddress()) : 0; }

    [[nodiscard]] metal_binding_group_layout const& layout() const { return *_layout; }

    /// The buffers this group names, for the command list to declare access on before a dispatch.
    [[nodiscard]] cc::span<sg::raw_buffer_handle const> bound_buffers() const { return _bound_buffers; }

    /// The textures this group names, held for the same reason the buffers are.
    [[nodiscard]] cc::span<sg::raw_texture_handle const> bound_textures() const { return _bound_textures; }

private:
    metal_context& _ctx;
    metal_binding_group_layout_handle _layout;
    MTL::Buffer* _arguments = nullptr;
    cc::vector<sg::raw_buffer_handle> _bound_buffers;
    cc::vector<sg::raw_texture_handle> _bound_textures;
};
