#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/backends/metal/metal_acceleration_structure.hh>
#include <shaped-graphics/backends/metal/metal_binding_group.hh>
#include <shaped-graphics/backends/metal/metal_binding_layout.hh>
#include <shaped-graphics/backends/metal/metal_buffer.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>
#include <shaped-graphics/backends/metal/metal_staging_binding_group.hh>
#include <shaped-graphics/backends/metal/metal_texture.hh>
#include <shaped-graphics/binding/impl/layout_hash.hh>

// Building binding group layouts, pipeline layouts and binding groups.
//
// The validation here is not shared with the other backends, and deliberately so — the coding guidelines'
// duplicate-rather-than-abstract stance.
// What IS shared is the vocabulary it validates against: `sg::accepts`, `binding_type` and the view kinds, so the rules
// being checked are the same ones dx12 and vulkan check.

namespace sg::backend::metal
{
namespace
{
/// One 8-byte slot of an argument buffer.
/// Every binding kind is this wide: a buffer is a GPU address, a texture or a sampler an MTLResourceID.
using argument_slot = u64;

constexpr isize k_argument_slot_size = isize(sizeof(argument_slot));

/// The slot index for element `element` of the binding at `b`.
[[nodiscard]] isize slot_of(sg::binding const& b, isize element)
{
    return isize(b.index) + element;
}
} // namespace

cc::result<metal_binding_group_layout_handle> metal_context::create_metal_binding_group_layout(
    cc::span<sg::binding const> bindings,
    cc::span<sg::named_sampler const> static_samplers,
    sg::lifetime_scope)
{
    // Rejected here rather than left to the argument-buffer write, where it would silently alias two bindings onto one
    // slot.
    // sg's rule is that `index` is the address within its group — see libs/graphics/shaped-graphics/docs/concepts/bindings.md — and Metal, like
    // SPIR-V and WGSL, has exactly one namespace per group, so a collision is not a layout it can express at all.
    for (auto i = isize(0); i < bindings.size(); ++i)
        for (auto j = i + 1; j < bindings.size(); ++j)
            if (bindings[i].index == bindings[j].index)
                return cc::error(cc::format("binding_group_layout: '{}' and '{}' share index {} — an index is unique "
                                            "within its group",
                                            bindings[i].name, bindings[j].name, bindings[i].index));

    for (auto const& b : bindings)
        if (b.count == 0)
            return cc::error(cc::format("binding_group_layout: '{}' is an unbounded array, which sg rejects", b.name));

    auto const hash = sg::impl::binding_group_layout_hash(bindings, static_samplers);
    return std::make_shared<metal_binding_group_layout const>(
        hash, cc::vector<sg::binding>::create_copy_of(bindings),
        cc::vector<sg::named_sampler>::create_copy_of(static_samplers));
}

cc::result<metal_pipeline_layout_handle> metal_context::create_metal_pipeline_layout(
    sg::pipeline_layout_description const& desc,
    sg::lifetime_scope)
{
    // The inline-constants block is staged and bound as an ordinary buffer here, so its size is what the stage
    // allocates rather than a root-signature parameter — and a block nobody sized would stage nothing.
    if (desc.inline_constants.has_value())
    {
        auto const& ic = desc.inline_constants.value();
        if (ic.type != sg::binding_type::constants_buffer)
            return cc::error("pipeline_layout: the inline_constants binding must be a constants buffer");
        if (!ic.block_size.has_value() || ic.block_size.value() <= 0 || ic.block_size.value() % 4 != 0)
            return cc::error("pipeline_layout: the inline_constants binding needs a block_size that is positive and a "
                             "multiple of 4");
    }

    auto const hash = sg::impl::pipeline_layout_hash(desc);
    return std::make_shared<metal_pipeline_layout const>(hash, desc);
}

cc::result<sg::staging_binding_group_handle> metal_context::create_metal_staging_binding_group(
    sg::binding_group_layout_handle layout,
    sg::lifetime_scope)
{
    CC_ASSERT(layout != nullptr, "staging_binding_group requires a binding_group_layout");
    auto const& typed_layout = static_cast<metal_binding_group_layout const&>(*layout);

    auto const bindings = typed_layout.bindings();

    // One offset per binding, in declaration order.
    // A binding's descriptor sits at its own index, because an argument buffer has a single slot space rather than
    // dx12's split view and sampler heaps — and sg already guarantees the index is unique within the group.
    //
    // A static sampler gets -1: the layout fixes its value, so there is no descriptor here for a setter to reach, and
    // the mint writes it in from the layout instead.
    auto offsets = cc::vector<int>::create_defaulted(bindings.size());
    for (auto i = isize(0); i < bindings.size(); ++i)
    {
        auto is_static = false;
        for (auto const& ns : typed_layout.static_samplers())
            if (ns.name == bindings[i].name)
                is_static = true;

        offsets[i] = is_static ? -1 : int(bindings[i].index);
    }

    return sg::staging_binding_group_handle(std::make_shared<metal_staging_binding_group>(
        *this, cc::move(layout), cc::move(offsets), typed_layout.argument_slot_count()));
}

cc::result<metal_binding_group_handle> metal_context::create_metal_binding_group(
    sg::binding_group_layout_handle const& layout,
    cc::span<sg::named_view const> views,
    cc::span<sg::named_sampler const> samplers,
    sg::lifetime_scope)
{
    CC_ASSERT(layout != nullptr, "binding_group requires a binding_group_layout");
    auto typed_layout = std::static_pointer_cast<metal_binding_group_layout const>(layout);

    auto const bindings = typed_layout->bindings();
    auto const slot_count = typed_layout->argument_slot_count();

    auto const scope = autorelease_scope();

    // Zero-initialized, so a slot nothing writes reads as a null descriptor rather than as whatever was there.
    auto slots = cc::vector<argument_slot>::create_filled(slot_count, argument_slot(0));
    auto filled = cc::vector<char>::create_filled(bindings.size(), char(0));
    auto bound_buffers = cc::vector<metal_binding_group::bound_buffer>();
    auto bound_textures = cc::vector<metal_binding_group::bound_texture>();
    auto bound_tlases = cc::vector<sg::tlas_handle>();
    auto array_bindings = cc::vector<metal_binding_group::array_binding>();

    auto const find_binding = [&](cc::string_view name) -> isize
    {
        for (auto i = isize(0); i < bindings.size(); ++i)
            if (bindings[i].name == name)
                return i;
        return -1;
    };

    for (auto const& nv : views)
    {
        auto const index = find_binding(nv.name);
        if (index < 0)
            return cc::error(cc::format("binding_group: no view binding named '{}' in the layout", nv.name));

        auto const& b = bindings[index];
        if (b.type == sg::binding_type::sampler)
            return cc::error(
                cc::format("binding_group: '{}' is a sampler binding and takes a sampler, not a view", b.name));

        auto const provided = nv.view.span();
        auto const expected = isize(b.count < 1 ? 1 : b.count);
        if (provided.size() != expected)
            return cc::error(
                cc::format("binding_group: '{}' takes {} view(s), {} provided", b.name, expected, provided.size()));

        CC_ASSERT(filled[index] == char(0), "binding_group: a binding was provided more than once");
        filled[index] = char(1);

        // An array binding's elements are collected rather than auto-declared: which of them a dispatch indexes is
        // the caller's to say through declare_array_*_access, and nothing here can infer it.
        //
        // An acceleration-structure array is the exception, and stays auto-declared: a trace reads every structure the
        // table can reach, so there is nothing for a per-element declare to narrow — and sg's two declare calls are
        // split by buffer and texture, with no third for this kind to arrive through.
        auto const shape = sg::shape_of(b.type);
        auto const is_array = b.is_array() && shape != sg::view_shape::acceleration_structure;
        auto array = metal_binding_group::array_binding{.name = cc::string(b.name),
                                                        .is_texture = shape == sg::view_shape::texture,
                                                        .elements = {}};

        for (auto element = isize(0); element < provided.size(); ++element)
        {
            auto const& view = provided[element];

            if (sg::is_vacant(view))
            {
                if (expected == 1)
                    return cc::error(cc::format("binding_group: '{}' — a vacant element is only valid in an array "
                                                "binding",
                                                b.name));
                if (is_array)
                    array.elements.push_back({}); // the zero already there IS the null descriptor
                continue;
            }

            if (!sg::accepts(b, view))
                return cc::error(
                    cc::format("binding_group: '{}' — the bound view does not match the binding's type", b.name));

            // An acceleration structure binds by resource id, exactly as a texture does — MTL::AccelerationStructure
            // is a resource of its own rather than a buffer, so there is no address to take.
            // A null one is the value every ray misses, and the zero already in the slot is what it encodes to.
            if (auto const* const tlas_view = sg::try_as_tlas_view(view); tlas_view != nullptr)
            {
                if (tlas_view->tlas == nullptr)
                    continue;

                auto const& mtl_tlas = static_cast<metal_tlas const&>(*tlas_view->tlas);
                if (mtl_tlas.storage().accel() == nullptr)
                    return cc::error(
                        cc::format("binding_group: '{}' — the bound acceleration structure has expired", b.name));

                slots[slot_of(b, element)] = mtl_tlas.storage().resource_id()._impl;
                bound_tlases.push_back(tlas_view->tlas);
                continue;
            }

            // A texture binds by resource id rather than by address — an argument buffer slot is the same 8 bytes
            // either way.
            if (auto const* const texture_view = sg::try_as_texture_view(view); texture_view != nullptr)
            {
                auto* const bound = _texture_views.acquire(*texture_view);
                if (bound == nullptr)
                    return cc::error(cc::format("binding_group: '{}' — the bound texture has no storage", b.name));

                slots[slot_of(b, element)] = bound->gpuResourceID()._impl;
                if (is_array)
                    array.elements.push_back({.texture = texture_view->texture});
                else
                    bound_textures.push_back({.texture = texture_view->texture, .bound_as = sg::view_class_of(view)});
                continue;
            }

            auto const* const buffer_view = sg::try_as_buffer_view(view);
            if (buffer_view == nullptr)
                return cc::error(cc::format(
                    "binding_group: '{}' — the bound view is of a kind the metal backend does not handle", b.name));
            if (buffer_view->buffer == nullptr)
                return cc::error(cc::format("binding_group: '{}' — a view always binds a resource", b.name));

            auto const& mtl_buffer = static_cast<metal_buffer const&>(*buffer_view->buffer);

            // The address the shader reads through, already offset: MSL indexes from the pointer it is given, so the
            // view's offset has to be folded in here rather than carried alongside.
            slots[slot_of(b, element)] = mtl_buffer.gpu_address() + u64(buffer_view->offset_in_bytes);
            if (is_array)
                array.elements.push_back({.buffer = buffer_view->buffer});
            else
                bound_buffers.push_back({.buffer = buffer_view->buffer, .bound_as = sg::view_class_of(view)});
        }

        if (is_array)
            array_bindings.push_back(cc::move(array));
    }

    // Static samplers first, so a dynamic one for the same name is rejected rather than silently overriding it.
    for (auto const& ns : typed_layout->static_samplers())
    {
        auto const index = find_binding(ns.name);
        if (index < 0)
            continue;

        auto* const state = _samplers.acquire(_device, ns.sampler);
        if (state == nullptr)
            return cc::error(cc::format("binding_group: the metal device refused a sampler state for '{}'", ns.name));
        slots[slot_of(bindings[index], 0)] = state->gpuResourceID()._impl;
        filled[index] = char(1);
    }

    for (auto const& ns : samplers)
    {
        auto const index = find_binding(ns.name);
        if (index < 0)
            return cc::error(cc::format("binding_group: no sampler binding named '{}' in the layout", ns.name));

        auto const& b = bindings[index];
        if (b.type != sg::binding_type::sampler)
            return cc::error(cc::format("binding_group: '{}' is not a sampler binding", b.name));
        if (filled[index] != char(0))
            return cc::error(cc::format("binding_group: sampler '{}' is static — it is fixed by the layout and must "
                                        "not be provided",
                                        b.name));

        auto* const state = _samplers.acquire(_device, ns.sampler);
        if (state == nullptr)
            return cc::error(cc::format("binding_group: the metal device refused a sampler state for '{}'", ns.name));
        slots[slot_of(b, 0)] = state->gpuResourceID()._impl;
        filled[index] = char(1);
    }

    for (auto i = isize(0); i < bindings.size(); ++i)
        if (filled[i] == char(0))
            return cc::error(cc::format("binding_group: binding '{}' was not provided", bindings[i].name));

    // An empty layout still gets a buffer: an argument table binds an address, and a null one is not a legal binding.
    auto const byte_size = cc::max(isize(k_argument_slot_size), slot_count * k_argument_slot_size);
    auto* const arguments = _device->newBuffer(
        NS::UInteger(byte_size), MTL::ResourceStorageModeShared | MTL::ResourceHazardTrackingModeUntracked);
    if (arguments == nullptr)
        return cc::error("binding_group: the metal device refused the argument buffer");

    arguments->setLabel(ns_string("sg binding group"));
    if (slot_count > 0)
        cc::memcpy(arguments->contents(), slots.data(), size_t(slot_count * k_argument_slot_size));

    _residency.add(arguments);

    return std::make_shared<metal_binding_group const>(*this, cc::move(typed_layout), arguments,
                                                       cc::move(bound_buffers), cc::move(bound_textures),
                                                       cc::move(bound_tlases), cc::move(array_bindings));
}
} // namespace sg::backend::metal
