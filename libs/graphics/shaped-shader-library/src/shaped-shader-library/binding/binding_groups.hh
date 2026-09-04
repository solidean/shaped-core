#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/sampler.hh>
#include <shaped-shader-library/fwd.hh>

/// A sampler the shader marked `static`, and the state it declared.
///
/// A static sampler is baked into the pipeline layout's root signature rather than given a descriptor, so it
/// costs no per-group descriptor and never varies frame to frame.
/// It still occupies its slot and its register: only where the state comes from changes.
struct slib::declared_sampler
{
    cc::string name;
    sg::sampler sampler;
};

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

    /// The subset of `bindings` the shader marked `static`, with the state each declared.
    /// A sampler binding absent from here is dynamic: the group supplies it, not the layout.
    cc::vector<declared_sampler> static_samplers;
};

/// The inline-constants block a shader declares:
///
///     #pragma sc push_constants space=9
///     ConstantBuffer<frame_constants> frame;
///
/// Inline constants — dx12 root constants, Vulkan push constants — reach a shader through
/// `pipeline_layout_description::inline_constants` rather than through a group.
/// The register is always `b0`, since a pipeline layout carries at most one such binding, so the only number to
/// state is the space — and stating it is the point, because a block sharing a space with a group's `b`
/// registers is exactly the collision this pass exists to prevent.
///
/// `block_size` is not here: it keeps coming from reflection, which is how a routine reads it today.
struct slib::shader_inline_constants
{
    cc::string name;
    u32 space = 0;
};

/// One member of an annotated struct, as the shader declares it.
struct slib::shader_struct_member
{
    cc::string name;
    cc::string type;     ///< the HLSL type, verbatim
    cc::string semantic; ///< the semantic without its trailing index, or empty when the member carries none
    u32 semantic_index = 0;
};

/// A vertex input struct: what feeds one bound vertex buffer.
///
///     #pragma sc vertex_input
///     struct vs_input
///     {
///         float3 position : POSITION;
///         float3 normal : NORMAL;
///     };
///
/// Members are numbered by declaration order, which is safe here in a way it is not for a group: a vertex input
/// struct is declared once, in one block, where a group has to survive being shared across files.
/// That number is what SPIR-V matches on, since it has no semantics at all.
///
/// The buffer's byte layout is the generated C++ mirror's rather than this struct's — a vertex buffer is a byte
/// stream the input assembler decodes per attribute offset, and the mirror is what a caller fills.
struct slib::shader_vertex_input
{
    cc::string name;
    u32 slot = 0; ///< which bound vertex buffer, matching sg::vertex_input_layout::create's argument position
    bool per_instance = false; ///< advance per instance rather than per vertex
    cc::vector<shader_struct_member> members;
};

/// A ray payload: the struct that travels through `TraceRay` to the hit and miss shaders and back.
///
///     #pragma sc payload
///     struct pt_payload { float3 radiance; float3 throughput; uint rng; };
///
/// `raytracing_pipeline_description::max_payload_size` is a byte count written by hand in C++ today, against a
/// struct in another language, and nothing in `sg::compiled_shader` reports one — so the pass computes it.
///
/// **A payload packs at natural alignment, not in a constant buffer's 16-byte rows**, which the spike's Q13
/// pins rather than assumes: `CreateStateObject` accepts the natural size and refuses one field less.
/// That is also why the generated C++ mirror needs no padding members: naturally packed is what C++ does.
struct slib::shader_payload
{
    cc::string name;
    cc::vector<shader_struct_member> members;
    isize size = 0; ///< what max_payload_size must be, computed from the members
};

/// Everything the pass reads out of one translation unit.
///
/// One result rather than a function per attribute, because they come out of one parse: a source is read once,
/// and a caller that wants two of these never risks two readings of it.
struct slib::shader_bindings
{
    cc::vector<shader_binding_group> groups;

    /// At most one, because a pipeline layout carries at most one inline-constants binding.
    cc::optional<shader_inline_constants> inline_constants;

    /// One per annotated vertex input struct, in declaration order.
    cc::vector<shader_vertex_input> vertex_inputs;

    /// One per annotated ray payload struct, in declaration order.
    cc::vector<shader_payload> payloads;
};

namespace slib
{
/// Everything `hlsl` declares, in declaration order.
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
/// Every attribute the grammar knows is honoured; a name outside it is an error naming the line.
[[nodiscard]] cc::result<shader_bindings> parse_binding_groups(cc::string_view hlsl);

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
