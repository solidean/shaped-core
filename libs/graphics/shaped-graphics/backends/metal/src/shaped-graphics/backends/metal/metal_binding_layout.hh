#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/binding/binding_group.hh> // sg::named_sampler, which the static-sampler list holds by value
#include <shaped-graphics/binding/binding_group_layout.hh>
#include <shaped-graphics/binding/pipeline_layout.hh>
#include <shaped-graphics/fwd.hh>

/// Metal implementation of sg::binding_group_layout.
///
/// **Metal has no descriptor-set layout object**, so this holds schema and nothing else — no device call is made and
/// `release_backend_objects` has nothing to do.
/// dx12 builds a root signature and vulkan a VkDescriptorSetLayout; here the shape only has to be agreed between the
/// group that writes an argument buffer and the shader that reads it.
///
/// **The argument-buffer layout is `binding.index`, directly.**
/// A group becomes one argument buffer, and each binding occupies the 8-byte slot at its own index — which is exactly
/// what `[[id(n)]]` addresses in MSL, and exactly what SPIRV-Cross emits for a descriptor set.
/// Every kind is 8 bytes: a buffer is a GPU address, a texture or sampler an `MTLResourceID`.
/// An array binding occupies `count` consecutive slots from its index.
class sg::backend::metal::metal_binding_group_layout final : public sg::binding_group_layout
{
public:
    metal_binding_group_layout(cc::hash128 structural_hash,
                               cc::vector<sg::binding> bindings,
                               cc::vector<sg::named_sampler> static_samplers)
      : sg::binding_group_layout(structural_hash, cc::move(bindings)), _static_samplers(cc::move(static_samplers))
    {
    }

    /// The samplers the layout fixes, matched to bindings by name.
    /// A group may not supply one of these; the layout's value is what the argument buffer gets.
    [[nodiscard]] cc::span<sg::named_sampler const> static_samplers() const { return _static_samplers; }

    /// Slots the argument buffer needs: one past the highest index any binding occupies.
    [[nodiscard]] isize argument_slot_count() const;

private:
    cc::vector<sg::named_sampler> _static_samplers;
};

/// Metal implementation of sg::pipeline_layout.
///
/// Also schema only: Metal has no root signature, and which argument buffer sits at which argument-table slot is
/// decided by the group's position here — slot `i` is the `group_index` a caller passes to `bind_group`.
class sg::backend::metal::metal_pipeline_layout final : public sg::pipeline_layout
{
public:
    metal_pipeline_layout(cc::hash128 structural_hash, sg::pipeline_layout_description desc)
      : sg::pipeline_layout(structural_hash), _desc(cc::move(desc))
    {
    }

    [[nodiscard]] sg::pipeline_layout_description const& description() const { return _desc; }

private:
    sg::pipeline_layout_description _desc;
};
