#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-shader-library/fwd.hh>

namespace slib::impl
{
/// What one HLSL value type means to the generated C++ that mirrors it.
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

    /// Where a constant block may start it, which is not the same number as its size.
    ///
    /// A 32-bit scalar or vector aligns to 4 and is kept off a row boundary by the no-straddling rule instead;
    /// a 64-bit scalar aligns to 8; a 64-bit vector and a matrix start a whole row.
    /// Q14i measured the 64-bit rules and Q14g the matrix, because none of them follows from the 32-bit ones.
    isize constant_block_align = 4;

    /// What C++ would align the mirror's member to, which is what a naturally packed payload uses.
    /// Differs from `constant_block_align` wherever the mirror is an array: `double[2]` aligns to 8 in C++ and
    /// starts a 16-byte row in a constant block.
    isize cpp_align = 4;

    /// How a vertex buffer would carry it, or nothing for a type that is not a vertex attribute.
    /// Most of the table has none: `sg::vertex_attribute_format` covers the 32-bit scalars and vectors, so
    /// `bool`, every 64-bit type, every narrow float and the matrix are constant-block and payload types only.
    cc::optional<sg::vertex_attribute_format> format;
};

/// The table entry for one HLSL value type, or nothing when the pass does not know it.
///
/// The coverage and the exclusions are one list, in
/// libs/graphics/shaped-shader-library/docs/binding-preprocessor.md's "The supported subset" — every exclusion
/// naming the measurement behind it rather than a policy.
/// The two that bite most often: only `float4xC` matrices are carried, because only a full float4 column has
/// one extent on every target (Q14g), and `half` is 32-bit storage here because nothing passes
/// `-enable-16bit-types` (Q14h).
[[nodiscard]] cc::optional<hlsl_value_type> value_type_of(cc::string_view hlsl_type);

/// Whether `hlsl_type` names a matrix, whether or not the table carries that shape.
///
/// The pass writes `column_major` before every matrix it parses, so it has to recognise one before deciding
/// whether it knows it — see `matrix_offsets` in binding_groups.cc.
[[nodiscard]] bool is_matrix_type(cc::string_view hlsl_type);

/// Whether `name` is an sg::vertex_attribute_format enumerator, spelled exactly.
///
/// What a `#pragma sc attribute format=<name>` may say, and the reason that attribute exists: HLSL has no
/// spelling that tells a `float4` fed by four floats from one fed by four normalized bytes, so a packed format
/// cannot be derived from the member's type and the author states it.
[[nodiscard]] bool is_vertex_attribute_format(cc::string_view name);

/// Why a type outside the table is outside it, as a sentence to append to a refusal — or empty when the pass
/// has nothing more specific to say than "not a type this pass knows".
///
/// A reader hitting a refusal wants opposite reactions to a table gap and to a portability rule, and the
/// message is the only thing that tells them which they have.
[[nodiscard]] cc::string_view rejection_reason_for(cc::string_view hlsl_type);
} // namespace slib::impl
