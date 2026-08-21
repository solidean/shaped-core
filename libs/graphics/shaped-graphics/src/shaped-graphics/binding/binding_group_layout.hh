#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/binding/binding.hh>
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

    /// The reflected bindings this schema was built from, in the order they were declared — sampler bindings included, static ones among them.
    /// A binding's position here is its *slot index*, the address a staging_binding_group resolves a name to.
    [[nodiscard]] cc::span<binding const> bindings() const { return _bindings; }

    /// The group index the bindings pin this layout to, inherited from them at creation — nothing if none of them declares one.
    /// Present means this layout may only ever be bound at that one slot, which every backend's `bind_group` checks.
    [[nodiscard]] cc::optional<u32> group_index() const { return _group_index; }

protected:
    /// `structural_hash` must come from sg::impl::binding_group_layout_hash over the creation arguments, and `bindings` must be the span it hashed.
    binding_group_layout(cc::hash128 structural_hash, cc::vector<binding> bindings)
      : _structural_hash(structural_hash), _bindings(cc::move(bindings)), _group_index(group_index_of(_bindings))
    {
    }

    cc::hash128 _structural_hash;
    cc::vector<binding> _bindings;
    cc::optional<u32> _group_index;
};
