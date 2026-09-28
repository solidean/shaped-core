#pragma once

#include <clean-core/error/result.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-shader-library/binding/impl/hlsl_tokens.hh>
#include <shaped-shader-library/fwd.hh>

namespace slib::impl
{
/// A storage texture's format, as `#pragma sc format rgba8_unorm` names it before an `RWTexture*`.
struct storage_format
{
    sg::pixel_format format = sg::pixel_format::undefined;
    /// What DXC's `[[vk::image_format]]` takes; empty where SPIR-V has no such format, as for `bgra8_unorm`.
    cc::string_view vulkan;
};

/// The format a `#pragma sc format ...` attribute names: one positional argument, `sg::pixel_format`'s own name.
/// Only a format a storage texture can have is accepted, `sg::supports_typed_uav`.
[[nodiscard]] cc::result<storage_format> parse_storage_format(annotation const& attribute);
} // namespace slib::impl
