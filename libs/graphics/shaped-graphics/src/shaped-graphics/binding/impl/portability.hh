#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/views.hh>

// A form a device lacks the feature for is refused here, before any backend sees it, so every backend refuses it alike.
// Each judgement takes the one feature's answer rather than the context, so a device lacking it is testable anywhere.
// See libs/graphics/shaped-graphics/docs/concepts/bindings.md, "Features".
namespace sg::impl
{
/// A storage binding whose format needs `feature::extended_storage_formats`, as a message, where `extended_storage_formats` is false.
[[nodiscard]] cc::optional<cc::string> find_unsupported_binding(bool extended_storage_formats,
                                                                cc::span<binding const> bindings);

/// A storage texture whose format needs `feature::extended_storage_formats`, as a message, where `extended_storage_formats` is false.
[[nodiscard]] cc::optional<cc::string> find_unsupported_texture(bool extended_storage_formats,
                                                                texture_description const& desc);

/// A 32-bit float view bound to a `filterable_float` binding, as a message, where `float32_filtering` is false.
/// A view of format `undefined` is judged by its texture's own format, which is what it reads as.
/// A binding with no sample type is not judged: a layout reflected from HLSL states none, and only WebGPU reads one.
[[nodiscard]] cc::optional<cc::string> find_unsupported_view(bool float32_filtering,
                                                             binding const& b,
                                                             cc::span<raw_view const> views);
[[nodiscard]] cc::optional<cc::string> find_unsupported_view(bool float32_filtering,
                                                             cc::span<binding const> bindings,
                                                             cc::span<named_view const> views);
[[nodiscard]] cc::optional<cc::string> find_unsupported_view(bool float32_filtering,
                                                             cc::span<binding const> bindings,
                                                             cc::span<slotted_view const> views);
} // namespace sg::impl
