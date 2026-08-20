#pragma once

#include <clean-core/bytes/hash128.hh>
#include <shaped-graphics/fwd.hh>

/// The frozen schema of one bindable resource group/set: built from a shader's `binding`s, composed into a pipeline_layout, and instantiated by binding_groups.
/// Held via binding_group_layout_handle.
///
/// Abstract: a backend subclasses it and owns the native object (dx12 descriptor-table schema, vulkan
/// VkDescriptorSetLayout). See libs/graphics/shaped-graphics/docs/concepts/bindings.md.
class sg::binding_group_layout
{
public:
    virtual ~binding_group_layout();

    /// Content identity: a hash over the bindings and static samplers this was created from, never over its own address.
    /// Stable across processes, which is what lets it key a cache that outlives one.
    ///
    /// Not virtual on purpose: identity is decided at the sg level, from sg-level inputs, so two backends cannot
    /// disagree about which layouts are the same one.
    [[nodiscard]] cc::hash128 structural_hash() const { return _structural_hash; }

protected:
    /// `structural_hash` must come from sg::impl::binding_group_layout_hash over the creation arguments.
    explicit binding_group_layout(cc::hash128 structural_hash) : _structural_hash(structural_hash) {}

    cc::hash128 _structural_hash;
};
