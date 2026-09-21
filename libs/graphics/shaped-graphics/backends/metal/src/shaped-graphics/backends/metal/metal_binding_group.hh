#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/fwd.hh>

/// Metal implementation of sg::binding_group: one argument buffer, and what it keeps alive.
///
/// **An argument buffer is a flat array of 8-byte slots**, indexed by `binding.index`, which is what `[[id(n)]]`
/// addresses in MSL.
/// A buffer binding holds a GPU address, and a texture, sampler or acceleration structure an `MTLResourceID`.
/// Nothing needs an encoder: the values are written straight into shared memory at creation.
///
/// The group holds every resource it names.
/// A caller may drop its own handle the moment the group is built — an argument buffer is raw addresses, with nothing
/// to keep the target alive — and the group's GPU work still has to find something there.
class sg::backend::metal::metal_binding_group final : public sg::binding_group
{
public:
    /// One element of an array binding: whichever resource is bound at that index, or neither where it is vacant.
    struct array_element
    {
        sg::raw_buffer_handle buffer;
        sg::raw_texture_handle texture;

        [[nodiscard]] bool is_vacant() const { return buffer == nullptr && texture == nullptr; }
    };

    /// One array binding of this group, under the name `declare_array_*_access` addresses it by.
    ///
    /// **An array's elements are the one thing a dispatch cannot infer.** Which of them a shader indexes is decided by
    /// data the backend never sees, so they are kept apart from the scalar bindings below and declared by the caller.
    struct array_binding
    {
        cc::string name;
        bool is_texture = false;
        cc::vector<array_element> elements;
    };

    metal_binding_group(metal_context& ctx,
                        metal_binding_group_layout_handle layout,
                        MTL::Buffer* arguments,
                        cc::vector<sg::raw_buffer_handle> bound_buffers,
                        cc::vector<sg::raw_texture_handle> bound_textures = {},
                        cc::vector<sg::tlas_handle> bound_tlases = {},
                        cc::vector<array_binding> array_bindings = {});
    ~metal_binding_group() override;

    /// The address an argument table binds this group at.
    [[nodiscard]] u64 argument_address() const { return _arguments != nullptr ? u64(_arguments->gpuAddress()) : 0; }

    [[nodiscard]] metal_binding_group_layout const& layout() const { return *_layout; }

    /// The buffers this group's *scalar* bindings name, for the command list to declare access on before a dispatch.
    /// An array binding's elements are not here — they are in `array_bindings()` and declared by the caller.
    [[nodiscard]] cc::span<sg::raw_buffer_handle const> bound_buffers() const { return _bound_buffers; }

    /// The textures this group's scalar bindings name, held for the same reason the buffers are.
    [[nodiscard]] cc::span<sg::raw_texture_handle const> bound_textures() const { return _bound_textures; }

    /// The acceleration structures this group names — a trace against one declares `accel_read` on it.
    [[nodiscard]] cc::span<sg::tlas_handle const> bound_tlases() const { return _bound_tlases; }

    /// The array bindings this group holds, each keeping its elements alive as the scalar lists do theirs.
    [[nodiscard]] cc::span<array_binding const> array_bindings() const { return _array_bindings; }

private:
    metal_context& _ctx;
    metal_binding_group_layout_handle _layout;
    MTL::Buffer* _arguments = nullptr;
    cc::vector<sg::raw_buffer_handle> _bound_buffers;
    cc::vector<sg::raw_texture_handle> _bound_textures;
    cc::vector<sg::tlas_handle> _bound_tlases;
    cc::vector<array_binding> _array_bindings;
};
