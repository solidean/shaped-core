#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/context/capabilities.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/views.hh>

// A form a device lacks the feature for is refused here, before any backend sees it, so every backend refuses it alike.
// Each judgement takes the features' answers rather than the context, so a device lacking one is testable anywhere.
// See libs/graphics/shaped-graphics/docs/concepts/bindings.md, "Features".
namespace sg::impl
{
/// A binding whose access its kind cannot carry, as a message — refused whatever the device.
/// Otherwise an image whose format needs `feature::extended_image_formats` where `supported` lacks it, or a read-write
/// image of a 16-bit normalized format where `supported` lacks `feature::readwrite_norm16_image_formats`.
[[nodiscard]] cc::optional<cc::string> find_unsupported_binding(feature_set supported, cc::span<binding const> bindings);

/// A texture with image usage whose format needs `feature::extended_image_formats`, as a message, where `extended_image_formats` is false.
/// A texture of a 16-bit normalized format needs it whatever its usage, since core WebGPU has no texture of one at all.
[[nodiscard]] cc::optional<cc::string> find_unsupported_texture(bool extended_image_formats,
                                                                texture_description const& desc);

/// A view bound to a `filterable_float` binding whose format `supported` cannot filter, as a message: a 32-bit float one
/// without `feature::float32_filtering`, or a 16-bit normalized one without `feature::norm16_filtering`.
/// Also a 16-bit normalized view bound to a read-write image binding without `feature::readwrite_norm16_image_formats`,
/// judged by the view's format whatever the binding states, so a layout reflected with no format is judged too.
/// A view of format `undefined` is judged by its texture's own format, which is what it reads as.
/// A binding with no sample type is not judged: a layout reflected from HLSL states none, and only WebGPU reads one.
[[nodiscard]] cc::optional<cc::string> find_unsupported_view(feature_set supported,
                                                             binding const& b,
                                                             cc::span<raw_view const> views);
[[nodiscard]] cc::optional<cc::string> find_unsupported_view(feature_set supported,
                                                             cc::span<binding const> bindings,
                                                             cc::span<named_view const> views);
[[nodiscard]] cc::optional<cc::string> find_unsupported_view(feature_set supported,
                                                             cc::span<binding const> bindings,
                                                             cc::span<slotted_view const> views);

/// The first stage whose `required_features` holds one outside `supported`, as a message naming the shader and the feature.
/// A null stage and a stage whose features are unknown are not judged.
[[nodiscard]] cc::optional<cc::string> find_missing_feature(feature_set supported,
                                                            cc::span<compiled_shader const* const> stages);

/// The first stage that declares a reserved slot, as a message naming the shader and the slot: sg lays out none, so a
/// pipeline of it is refused rather than left for the backend's own pipeline creation to fail on.
/// A null stage is not judged.
[[nodiscard]] cc::optional<cc::string> find_reserved_slot(cc::span<compiled_shader const* const> stages);
} // namespace sg::impl
