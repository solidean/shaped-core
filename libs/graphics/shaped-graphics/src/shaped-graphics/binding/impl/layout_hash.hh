#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/span.hh>
#include <shaped-graphics/binding/binding_group.hh>   // named_sampler
#include <shaped-graphics/binding/pipeline_layout.hh> // bound_sampler, pipeline_layout_description
#include <shaped-graphics/fwd.hh>

/// Content identity for the binding layouts, over what they were built FROM.
///
/// Never over a handle's address: a key that outlives the process has to mean the same thing in the next one, and an
/// address means nothing there.
/// A layout computes its hash once at creation and stores it, so using these on a hot path costs a load.
///
/// The in-memory pipeline_cache keys off the same functions, which is what keeps its tier and any persistent tier
/// from disagreeing about what "the same layout" is.

namespace sg::impl
{
/// Identity of a binding_group_layout: its bindings, and the static samplers baked into it.
[[nodiscard]] cc::hash128 binding_group_layout_hash(cc::span<binding const> bindings,
                                                    cc::span<named_sampler const> static_samplers);

/// Identity of a pipeline_layout: its groups' own identities, its register-bound static samplers, and its inline constants.
/// All three change the root signature, so all three are part of it.
[[nodiscard]] cc::hash128 pipeline_layout_hash(cc::span<binding_group_layout_handle const> groups,
                                               cc::span<bound_sampler const> static_samplers,
                                               cc::optional<binding> const& inline_constants);

[[nodiscard]] cc::hash128 pipeline_layout_hash(pipeline_layout_description const& desc);
} // namespace sg::impl
