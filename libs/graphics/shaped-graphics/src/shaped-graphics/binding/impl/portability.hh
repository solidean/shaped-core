#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/fwd.hh>

// A form a device lacks the feature for is refused here, before any backend sees it, so every backend refuses it alike.
// See libs/graphics/shaped-graphics/docs/concepts/bindings.md, "Features".
namespace sg::impl
{
/// A storage binding whose format needs `feature::extended_storage_formats`, as a message, where `ctx` lacks it.
[[nodiscard]] cc::optional<cc::string> find_unsupported_binding(context const& ctx, cc::span<binding const> bindings);

/// A storage texture whose format needs `feature::extended_storage_formats`, as a message, where `ctx` lacks it.
[[nodiscard]] cc::optional<cc::string> find_unsupported_texture(context const& ctx, texture_description const& desc);

/// A 32-bit float view bound to a `filterable_float` binding, as a message, where `ctx` lacks `feature::float32_filtering`.
/// A binding with no sample type is not judged: a layout reflected from HLSL states none, and only WebGPU reads one.
[[nodiscard]] cc::optional<cc::string> find_unsupported_view(context const& ctx,
                                                             binding_group_layout const& layout,
                                                             cc::span<named_view const> views);
[[nodiscard]] cc::optional<cc::string> find_unsupported_view(context const& ctx,
                                                             binding_group_layout const& layout,
                                                             cc::span<slotted_view const> views);
} // namespace sg::impl
