#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-shader-library/fwd.hh>

/// One binding group a shader declares, as an annotated namespace:
///
///     #pragma sc group 0
///     namespace frame_bindings
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
    u32 group = 0;   ///< the number the attribute carries, explicitly rather than by order of appearance

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
/// An attribute is a `#pragma sc <name> [key=value]...` standing on the line before what it applies to.
/// A pragma rather than a comment because DXC's include flatten erases comments and reproduces pragmas verbatim —
/// the spike's Q11 and Q12 pin both halves, and the pass reads the flattened source.
///
/// Reads the whole file rather than only annotated namespaces, because an attribute anywhere it cannot honour is
/// an error naming the line — a directive that silently does nothing is the failure this design exists to avoid,
/// and silently doing nothing is exactly what DXC makes of a pragma it does not know.
/// Text carrying no attribute is not interpreted at all, so a shader may keep hand-written `register()`
/// declarations at file scope indefinitely.
///
/// The supported subset inside an annotated namespace is small on purpose: `Type name;` and `Type name[N];`,
/// where `N` is a decimal literal and `Type` is a name from the pass's table, optionally with a flat `<...>`
/// argument list.
/// Everything else there is an error rather than something walked past.
///
/// An error names the file and line the author would recognise, which it recovers from the `#line` directives the
/// flatten leaves behind; a source carrying none is named by line alone.
/// `static`, `push_constants`, `payload` and `vertex_input` parse as attributes and are reported as not yet
/// supported, so a shader written against them fails rather than compiling to something that ignores them.
[[nodiscard]] cc::result<cc::vector<shader_binding_group>> parse_binding_groups(cc::string_view hlsl);

/// `hlsl` with every annotated binding carrying the address the pass assigns it: `register(t0, space0)` on the
/// DXIL arm, `[[vk::binding(0, 0)]]` on the SPIR-V arm, and with the pass's own pragmas removed.
///
/// Writing the address is the whole point rather than a convenience.
/// DXC numbers only what an entry point references, so without it two stages of one pipeline disagree about a
/// resource neither of them numbered — see the spike's Q8.
///
/// The pragmas go because DXC ignores an unknown one today, but `-Wall` promotes it to `-Wunknown-pragmas` and
/// `-WX` makes that an error — so the compiler is never handed a directive whose meaning it does not share.
///
/// Runs on the flattened translation unit, between the include flatten and the compile, because that is exactly
/// the scope a group's numbering is defined over.
/// Text carrying no attribute comes back byte for byte, so a source that declares no group is returned unchanged
/// whatever the target — the arm only has to exist for a source that needs one.
[[nodiscard]] cc::result<cc::string> rewrite_binding_groups(cc::string_view hlsl, sg::shader_format target);
} // namespace slib
