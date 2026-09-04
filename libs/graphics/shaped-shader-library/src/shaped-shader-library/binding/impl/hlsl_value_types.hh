#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-shader-library/fwd.hh>

namespace slib::impl
{
/// What one HLSL scalar or vector type means to the generated C++ that mirrors it.
///
/// A vertex buffer is a byte stream the input assembler decodes per attribute offset, so the layout is the
/// mirror struct's rather than the HLSL struct's: the generator emits a naturally packed C++ struct and
/// describes that same struct with `offsetof`.
/// Nothing here therefore models HLSL's constant-buffer packing, which a vertex input never sees.
struct hlsl_value_type
{
    cc::string_view cpp_type; ///< what the mirror declares, spelled without any helper the package cannot see
    isize size = 4;           ///< bytes, naturally packed
    sg::vertex_attribute_format format;
};

/// The table entry for one HLSL scalar or vector type, or nothing when the pass does not know it.
[[nodiscard]] cc::optional<hlsl_value_type> value_type_of(cc::string_view hlsl_type);
} // namespace slib::impl
