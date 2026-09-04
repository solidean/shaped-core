#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-shader-library/fwd.hh>

namespace slib::impl
{
/// What one HLSL scalar or vector type means to the generated C++ that mirrors it.
///
/// Two mirrors are generated from this one table, and they pack differently for different reasons.
/// A vertex buffer is a byte stream the input assembler decodes per attribute offset, so the mirror *defines*
/// the layout and everything is naturally packed.
/// A constant block's layout is DXC's, and the mirror has to reproduce it with explicit padding — the spike's
/// Q14 measures those rules rather than restating them.
struct hlsl_value_type
{
    cc::string_view cpp_type; ///< what the mirror declares, spelled without any helper the package cannot see
    isize size = 4;           ///< bytes the value itself occupies

    /// How a vertex buffer would carry it, or nothing for a type that is not a vertex attribute.
    /// `bool` is the case that has none: it is four bytes in a constant block and no attribute format at all.
    cc::optional<sg::vertex_attribute_format> format;
};

/// The table entry for one HLSL scalar or vector type, or nothing when the pass does not know it.
///
/// Matrices and arrays are deliberately absent, and Q14 is why: in a constant block a matrix whose rows are
/// not full `float4`s leaves a partial last row that the next member packs into, which C++ cannot express and
/// which SPIR-V rejects outright.
[[nodiscard]] cc::optional<hlsl_value_type> value_type_of(cc::string_view hlsl_type);
} // namespace slib::impl
