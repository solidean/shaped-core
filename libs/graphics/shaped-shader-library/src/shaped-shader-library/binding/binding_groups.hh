#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-shader-library/fwd.hh>

/// One binding group a shader declares, as an annotated namespace:
///
///     namespace frame_bindings //!> group 0
///     {
///         Texture2D<float4> albedo;
///         SamplerState linear_sampler;
///     }
///
/// The group number is both the SPIR-V set and the HLSL space, so group `n` occupies `space<n>` and nothing else does.
/// See libs/graphics/shaped-shader-library/docs/binding-preprocessor.md.
struct slib::shader_binding_group
{
    cc::string name; ///< the namespace's name, which is also what the generated struct is named after
    u32 group = 0;   ///< the number the annotation carries, explicitly rather than by order of appearance

    /// In declaration order, so a binding's position here is its slot in the layout this group defines.
    ///
    /// One counter per group runs across register classes, so a texture then a sampler take index 0 and 1 —
    /// `t0`/`s1` on DXIL against `binding(0)`/`binding(1)` on SPIR-V.
    /// The gaps that leaves in each DXIL class cost nothing: a register number is a name, not a position.
    ///
    /// An array binding consumes `count` indices, because DXIL numbers each element while SPIR-V numbers the
    /// array once — so the counter must advance by the length or the two targets disagree past it.
    /// Position and index are therefore the same number only in a group with no arrays.
    cc::vector<sg::binding> bindings;
};

namespace slib
{
/// Every binding group `hlsl` declares, in declaration order.
///
/// Reads the whole file rather than only annotated namespaces, because an attribute anywhere it cannot honour is
/// an error naming the line — a `//!>` that silently reads as a comment is the failure this design exists to avoid.
/// Text carrying no attribute is not interpreted at all, so a shader may keep hand-written `register()`
/// declarations at file scope indefinitely.
///
/// The supported subset inside an annotated namespace is small on purpose: `Type name;` and `Type name[N];`,
/// where `N` is a decimal literal and `Type` is a name from the pass's table, optionally with a flat `<...>`
/// argument list.
/// Everything else there is an error rather than something walked past.
///
/// The error names the line and what could not be parsed; the caller adds the file.
/// `static`, `push_constants`, `payload` and `vertex_input` parse as attributes and are reported as not yet
/// supported, so a shader written against them fails rather than compiling to something that ignores them.
[[nodiscard]] cc::result<cc::vector<shader_binding_group>> parse_binding_groups(cc::string_view hlsl);
} // namespace slib
