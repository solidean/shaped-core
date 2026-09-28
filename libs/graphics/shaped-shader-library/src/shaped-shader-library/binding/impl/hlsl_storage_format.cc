#include "hlsl_storage_format.hh"

#include <clean-core/string/format.hh>

namespace
{
struct named_format
{
    cc::string_view name;
    sg::pixel_format format;
    cc::string_view vulkan;
};

// Every format `sg::supports_typed_uav` allows, by sg's name, and DXC's spelling of it for SPIR-V.
constexpr named_format k_formats[] = {
    {"r8_unorm", sg::pixel_format::r8_unorm, "r8"},
    {"r8_snorm", sg::pixel_format::r8_snorm, "r8snorm"},
    {"r8_uint", sg::pixel_format::r8_uint, "r8ui"},
    {"r8_sint", sg::pixel_format::r8_sint, "r8i"},
    {"rg8_unorm", sg::pixel_format::rg8_unorm, "rg8"},
    {"rg8_snorm", sg::pixel_format::rg8_snorm, "rg8snorm"},
    {"rg8_uint", sg::pixel_format::rg8_uint, "rg8ui"},
    {"rg8_sint", sg::pixel_format::rg8_sint, "rg8i"},
    {"rgba8_unorm", sg::pixel_format::rgba8_unorm, "rgba8"},
    {"rgba8_snorm", sg::pixel_format::rgba8_snorm, "rgba8snorm"},
    {"rgba8_uint", sg::pixel_format::rgba8_uint, "rgba8ui"},
    {"rgba8_sint", sg::pixel_format::rgba8_sint, "rgba8i"},
    {"bgra8_unorm", sg::pixel_format::bgra8_unorm, ""},
    {"r16_float", sg::pixel_format::r16_float, "r16f"},
    {"r16_uint", sg::pixel_format::r16_uint, "r16ui"},
    {"r16_sint", sg::pixel_format::r16_sint, "r16i"},
    {"rg16_float", sg::pixel_format::rg16_float, "rg16f"},
    {"rg16_uint", sg::pixel_format::rg16_uint, "rg16ui"},
    {"rg16_sint", sg::pixel_format::rg16_sint, "rg16i"},
    {"rgba16_float", sg::pixel_format::rgba16_float, "rgba16f"},
    {"rgba16_uint", sg::pixel_format::rgba16_uint, "rgba16ui"},
    {"rgba16_sint", sg::pixel_format::rgba16_sint, "rgba16i"},
    {"r32_float", sg::pixel_format::r32_float, "r32f"},
    {"r32_uint", sg::pixel_format::r32_uint, "r32ui"},
    {"r32_sint", sg::pixel_format::r32_sint, "r32i"},
    {"rg32_float", sg::pixel_format::rg32_float, "rg32f"},
    {"rg32_uint", sg::pixel_format::rg32_uint, "rg32ui"},
    {"rg32_sint", sg::pixel_format::rg32_sint, "rg32i"},
    {"rgba32_float", sg::pixel_format::rgba32_float, "rgba32f"},
    {"rgba32_uint", sg::pixel_format::rgba32_uint, "rgba32ui"},
    {"rgba32_sint", sg::pixel_format::rgba32_sint, "rgba32i"},
    {"rgb10a2_unorm", sg::pixel_format::rgb10a2_unorm, "rgb10a2"},
    {"rg11b10_float", sg::pixel_format::rg11b10_float, "r11g11b10f"},
};
} // namespace

cc::result<slib::impl::storage_format> slib::impl::parse_storage_format(annotation const& attribute)
{
    auto const& args = attribute.arguments;
    if (args.size() != 1 || !args[0].key.empty() || args[0].values.size() != 1)
        return cc::error(cc::format("{}: 'format' takes one storage format, as sg::pixel_format names it: "
                                    "`#pragma sc format rgba8_unorm`",
                                    to_string(attribute.location)));
    for (auto const& f : k_formats)
        if (f.name == args[0].values[0])
            return storage_format{.format = f.format, .vulkan = f.vulkan};
    return cc::error(cc::format("{}: '{}' is no format a storage texture can have", to_string(attribute.location),
                                args[0].values[0]));
}
