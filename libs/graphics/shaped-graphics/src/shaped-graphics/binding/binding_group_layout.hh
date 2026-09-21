#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/sampler.hh>
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

    /// Destroys the backend objects now, leaving this an inert husk.
    ///
    /// The context calls this at shutdown for everything its pipeline cache still holds, because the cache dropping
    /// its own reference is NOT what frees them: a scheduled build node keeps the handle alive on a pool worker, and
    /// that node's later drop would run this destructor against a context that no longer exists.
    /// See libs/graphics/shaped-graphics/docs/concepts/caches.md, "Shutdown releases the objects, not just the entries".
    ///
    /// Idempotent, and the destructor afterwards does nothing.
    /// The default is empty, for a backend whose objects are reference-counted and safe to drop late.
    virtual void release_backend_objects() {}

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

    /// The static samplers baked into this schema, by the binding name each was given at creation.
    ///
    /// Kept because a caller creating a group against this layout has to know which of its own gathered samplers
    /// the layout already owns: dx12 refuses a static sampler supplied per group, so one passed anyway is an
    /// error rather than a duplicate.
    /// Part of the structural hash, so two layouts differing only here are different layouts.
    [[nodiscard]] cc::span<named_sampler const> static_samplers() const { return _static_samplers; }

protected:
    /// `structural_hash` must come from sg::impl::binding_group_layout_hash over the creation arguments, and `bindings` and `static_samplers` must be what it hashed.
    binding_group_layout(cc::hash128 structural_hash,
                         cc::vector<binding> bindings,
                         cc::vector<named_sampler> static_samplers)
      : _structural_hash(structural_hash),
        _bindings(cc::move(bindings)),
        _group_index(group_index_of(_bindings)),
        _static_samplers(cc::move(static_samplers))
    {
    }

    cc::hash128 _structural_hash;
    cc::vector<binding> _bindings;
    cc::optional<u32> _group_index;
    cc::vector<named_sampler> _static_samplers;
};

namespace sg::impl
{
/// Why a group cannot be bound at a slot: what the slot's layout holds and what the group's does, by their bindings' names.
///
/// A group fits a slot only if it was created against the very layout object the slot holds, and a hash names no shader.
/// Names do: an SGL group's bindings are `<binding>_<member>`, so the message says which declaration each side came from.
/// Built only when the check fails, since every backend's `bind_group` states it as a CC_ASSERTF argument.
[[nodiscard]] cc::string describe_layout_mismatch(int slot,
                                                  binding_group_layout const* expected,
                                                  binding_group_layout const* bound);
} // namespace sg::impl
