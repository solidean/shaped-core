#include "metal_staging_binding_group.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/backends/metal/metal_acceleration_structure.hh>
#include <shaped-graphics/backends/metal/metal_binding_group.hh>
#include <shaped-graphics/backends/metal/metal_binding_layout.hh>
#include <shaped-graphics/backends/metal/metal_buffer.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>

namespace sg::backend::metal
{
metal_staging_binding_group::metal_staging_binding_group(metal_context& ctx,
                                                         sg::binding_group_layout_handle layout,
                                                         cc::vector<int> descriptor_offsets,
                                                         isize slot_count)
  : sg::staging_binding_group(cc::move(layout), cc::move(descriptor_offsets)),
    _ctx(ctx),
    _slots(cc::vector<u64>::create_filled(slot_count, u64(0))),
    _resources(cc::vector<sg::raw_buffer_handle>::create_defaulted(slot_count)),
    _texture_resources(cc::vector<sg::raw_texture_handle>::create_defaulted(slot_count)),
    _tlas_resources(cc::vector<sg::tlas_handle>::create_defaulted(slot_count))
{
}

void metal_staging_binding_group::write_view_descriptors(int first_descriptor,
                                                         sg::binding const& b,
                                                         cc::span<sg::raw_view const> views)
{
    for (auto i = isize(0); i < views.size(); ++i)
    {
        auto const slot = isize(first_descriptor) + i;
        CC_ASSERT(slot >= 0 && slot < _slots.size(), "a staging descriptor index is out of the group's range");

        // An acceleration structure binds by resource id, exactly as a texture does.
        // A null one is a VALUE rather than an absence — it is what every ray misses — and in an argument buffer that
        // value is a zero slot, which is also what a vacant element reads as.
        if (auto const* const tlas_view = sg::try_as_tlas_view(views[i]); tlas_view != nullptr)
        {
            auto const* const mtl_tlas = static_cast<metal_tlas const*>(tlas_view->tlas.get());
            _slots[slot] = mtl_tlas != nullptr ? mtl_tlas->storage().resource_id()._impl : u64(0);
            _resources[slot] = nullptr;
            _texture_resources[slot] = nullptr;
            _tlas_resources[slot] = tlas_view->tlas;
            continue;
        }

        if (auto const* const texture_view = sg::try_as_texture_view(views[i]); texture_view != nullptr)
        {
            auto* const bound = _ctx.texture_views().acquire(*texture_view);
            _slots[slot] = bound != nullptr ? bound->gpuResourceID()._impl : u64(0);
            _resources[slot] = nullptr;
            _texture_resources[slot] = texture_view->texture;
            continue;
        }

        auto const* const buffer_view = sg::try_as_buffer_view(views[i]);
        // sg has already validated the view against the binding, so anything else is a kind this backend has not
        // reached rather than a caller error.
        CC_ASSERT(buffer_view != nullptr, "the metal backend cannot stage this view kind");

        auto const& mtl_buffer = static_cast<metal_buffer const&>(*buffer_view->buffer);

        // Offset folded into the address: MSL indexes from the pointer it is handed, so a view's offset cannot be
        // carried alongside it the way a descriptor's would be.
        _slots[slot] = mtl_buffer.gpu_address() + u64(buffer_view->offset_in_bytes);
        _resources[slot] = buffer_view->buffer;
        _texture_resources[slot] = nullptr;
    }
    (void)b;
}

void metal_staging_binding_group::clear_view_descriptors(int first_descriptor, sg::binding const& b, int count)
{
    for (auto i = 0; i < count; ++i)
    {
        auto const slot = isize(first_descriptor) + i;
        CC_ASSERT(slot >= 0 && slot < _slots.size(), "a staging descriptor index is out of the group's range");

        _slots[slot] = 0;
        _resources[slot] = nullptr; // releasing the reference is half of what clearing means
        _texture_resources[slot] = nullptr;
    }
    (void)b;
}

void metal_staging_binding_group::write_sampler_descriptor(int descriptor_index, sg::sampler const& smp)
{
    CC_ASSERT(descriptor_index >= 0 && descriptor_index < _slots.size(), "a staging sampler index is out of the "
                                                                         "group's range");

    // A refusal is recorded rather than reported here: this setter has no error channel, and `mint` is the point at
    // which a caller asks whether the group is usable.
    auto* const state = _ctx.samplers().acquire(_ctx.device(), smp);
    if (state == nullptr)
    {
        _sampler_refused = true;
        return;
    }
    _slots[descriptor_index] = state->gpuResourceID()._impl;
}

cc::result<sg::binding_group_handle> metal_staging_binding_group::mint()
{
    if (_sampler_refused)
        return cc::error("staging_binding_group: the metal device refused a sampler state for this group");

    auto const scope = autorelease_scope();

    auto const& typed_layout = static_cast<metal_binding_group_layout const&>(*layout());

    // A fresh buffer per snapshot, which is what makes snapshots independent of the builder and of each other: a
    // caller holding an old one keeps the descriptors it was minted with, whatever the builder does next.
    auto const byte_size = cc::max(isize(sizeof(u64)), _slots.size() * isize(sizeof(u64)));
    auto* const arguments = _ctx.device()->newBuffer(
        NS::UInteger(byte_size), MTL::ResourceStorageModeShared | MTL::ResourceHazardTrackingModeUntracked);
    if (arguments == nullptr)
        return cc::error("staging_binding_group: the metal device refused the argument buffer");

    arguments->setLabel(ns_string("sg staging snapshot"));

    auto slots = _slots;

    // The layout's static samplers are not staged — they have no descriptor a setter could reach — so they are written
    // into every snapshot from the layout itself.
    for (auto const& ns : typed_layout.static_samplers())
        for (auto const& b : typed_layout.bindings())
            if (b.name == ns.name)
            {
                auto* const state = _ctx.samplers().acquire(_ctx.device(), ns.sampler);
                if (state == nullptr)
                    return cc::error(cc::format("staging_binding_group: the metal device refused a sampler state "
                                                "for '{}'",
                                                ns.name));
                slots[isize(b.index)] = state->gpuResourceID()._impl;
            }

    if (!slots.empty())
        cc::memcpy(arguments->contents(), slots.data(), size_t(slots.size() * isize(sizeof(u64))));

    // Everything the snapshot names, deduplicated only by being a handle each: the group has to outlive the builder's
    // next mutation.
    //
    // **An array binding's elements are routed apart from the scalar ones**, exactly as the direct creation path routes
    // them, because a dispatch declares an array per element and a scalar binding automatically.
    // The slot is what connects the two: a binding owns `count` consecutive slots from its own index, so walking the
    // layout's bindings is what turns this flat image back into the shape a declare addresses by name.
    auto bound = cc::vector<sg::raw_buffer_handle>();
    auto bound_textures = cc::vector<sg::raw_texture_handle>();
    auto array_bindings = cc::vector<metal_binding_group::array_binding>();

    auto const slot_resource = [&](isize slot) -> metal_binding_group::array_element
    {
        auto out = metal_binding_group::array_element{};
        if (slot < _resources.size())
            out.buffer = _resources[slot];
        if (slot < _texture_resources.size())
            out.texture = _texture_resources[slot];
        return out;
    };

    for (auto const& b : typed_layout.bindings())
    {
        auto const shape = sg::shape_of(b.type);
        auto const count = isize(b.count < 1 ? 1 : b.count);
        auto const is_array = b.is_array() && shape != sg::view_shape::acceleration_structure;

        if (!is_array)
        {
            for (auto element = isize(0); element < count; ++element)
            {
                auto const resource = slot_resource(isize(b.index) + element);
                if (resource.buffer != nullptr)
                    bound.push_back(resource.buffer);
                if (resource.texture != nullptr)
                    bound_textures.push_back(resource.texture);
            }
            continue;
        }

        auto array = metal_binding_group::array_binding{.name = cc::string(b.name),
                                                        .is_texture = shape == sg::view_shape::texture,
                                                        .elements = {}};
        for (auto element = isize(0); element < count; ++element)
            array.elements.push_back(slot_resource(isize(b.index) + element));
        array_bindings.push_back(cc::move(array));
    }

    auto bound_tlases = cc::vector<sg::tlas_handle>();
    for (auto const& r : _tlas_resources)
        if (r != nullptr)
            bound_tlases.push_back(r);

    _ctx.residency().add(arguments);

    auto typed = std::static_pointer_cast<metal_binding_group_layout const>(layout());
    return sg::binding_group_handle(std::make_shared<metal_binding_group const>(
        _ctx, cc::move(typed), arguments, cc::move(bound), cc::move(bound_textures), cc::move(bound_tlases),
        cc::move(array_bindings)));
}
} // namespace sg::backend::metal
