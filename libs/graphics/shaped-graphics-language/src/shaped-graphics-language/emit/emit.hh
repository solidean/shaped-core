#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/check/check.hh>
#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/ir/dxil/signature.hh>
#include <shaped-graphics-language/legalize/legalize.hh>

/// The text a graphics API compiles, written from one flat entry point of a checked module.
///
/// A target is a text format together with the addressing rules of the backend that reads it.
/// So the two HLSL targets are two outputs: they differ in how a location, a group resource and the inline constants are addressed.
/// Every target carries its final addresses, so no later pass numbers what the text declares.
enum class sgl::emit::target : sgl::u8
{
    hlsl_dx12,
    hlsl_vulkan,
    wgsl,
    msl,
};

/// What a mesh entry point is written as (CHK-524): the stage itself, or one half of its polyfill.
enum class sgl::emit::mesh_form : sgl::u8
{
    native,
    /// The compute entry point that writes its workgroups' slots of `sg_mesh_output`.
    compute,
    /// Between a polyfilled task stage's compute entry point and a mesh stage's: the one that numbers the launched mesh
    /// workgroups in order; written from the mesh entry point, since the draw's vertex count is its.
    /// With no task stage, it numbers the one record an indirect draw writes.
    scan,
    /// The compute entry point an indirect draw of a pipeline with no task stage dispatches: `compute`, its workgroups
    /// launched by that one record.
    launched,
    /// The vertex entry point that draws those slots from `sg_mesh_input`.
    /// A pixel entry point is then its polyfilled pipeline's, which takes what the mesh stage wrote per primitive from
    /// the vertex half.
    vertex_fetch,
    /// A geometry entry point's mesh route on a target with mesh shaders: its pipeline's vertex stage and itself as one
    /// mesh entry point, which the pipeline draws with no buffer of its own (CHK-525).
    mesh,
};

/// What keeps an entry point from being written, as a closed set with stable kebab-case names.
enum class sgl::emit::error_kind : sgl::u8
{
    /// The module reported an error, so nothing about it is written, whichever entry point was asked for.
    module_has_errors,
    /// Not a position in `checked_module::entry_points`.
    unknown_entry_point,
    /// Unused since an entry point is renamed per target; kept so the ids of the kinds around it do not move.
    reserved_entry_point_name,
    /// A construct the emitters do not carry yet; never a guess at its address or its meaning.
    unsupported,
    /// A vertex input member whose semantic would start with `SV_`, which dx12 reads as a system value.
    system_value_semantic,
    /// Unused since every target is made to follow SGL's own layout; kept so the ids of the kinds after it do not move.
    layout_mismatch,
    /// A literal that is infinite or not a number, which no target can spell.
    non_finite_literal,
    /// A flat tree the check pass does not produce: an unfilled node, or a call of something that is no builtin function.
    malformed_tree,
    /// A flat tree in the structured form, which `check::legalize` has to take to the core form first.
    /// The detail is the first violation `check::find_core_violation` names.
    not_core,
    /// An entry point listing more groups than sg binds, which is three besides the inline constants.
    too_many_groups,
    /// An entry point that needs a feature no device of this target has, which the detail names.
    target_lacks_feature,
    /// A struct placed both in a constant block and in a storage buffer, whose two rules would give it two layouts.
    layout_conflict,
    /// A gap before a member of a `@no_padding` struct or binding, which the detail places.
    padding_forbidden,
    /// A file-scope sampler the entry point reaches whose index is past the 16 a stage holds on Metal and WebGPU.
    too_many_samplers,
    /// A WebGPU trace of an acceleration member at position 16 or later, past the 16 roots sg binds.
    too_many_acceleration_structures,
    /// A metal ray-tracing stage held by pipelines or hit groups whose ray sets size the ray data differently.
    ray_data_conflict,
};

struct sgl::emit::error
{
    error_kind kind = error_kind::unsupported;
    /// The declaration the error is about, which names its file and its place there; `none` when there is no such symbol.
    check::symbol_id symbol = check::symbol_id::none;
    /// What the kind alone cannot say, for a reader.
    cc::string detail;

    bool operator==(error const&) const = default;
};

/// One resource the text declares: the identifier it chose, and the name the host binds it by.
struct sgl::emit::bound_name
{
    /// As the text spells it, which is what the target's compiler reflects.
    cc::string emitted;
    /// `binding.member` for a resource, and its own name for a block of constants and for a file-scope sampler.
    cc::string host;

    bool operator==(bound_name const&) const = default;
};

/// One value of GPU memory as the text declares it: its name, dotted into a nested struct, and its offset from the start.
struct sgl::emit::emitted_field
{
    /// `l.t.color`, each step as the text spells the member.
    cc::string name;
    i32 offset = 0;

    bool operator==(emitted_field const&) const = default;
};

/// A constant block or a buffer's element, laid out as the text declares it, which a compiler reflecting the text reports.
/// It is SGL's layout as this target spells it: the fields of a memory form where the target needs one (memory_form.hh).
struct sgl::emit::emitted_layout
{
    /// The global the block or the buffer is read through.
    cc::string global;
    /// The bytes one element of a buffer takes; 0 for a block.
    i32 stride = 0;
    /// Every builtin value, in memory order; empty for a buffer of a builtin, which only its stride describes.
    cc::vector<emitted_field> fields;

    bool operator==(emitted_layout const& rhs) const
    {
        return global == rhs.global && stride == rhs.stride && ast::impl::is_equal(fields, rhs.fields);
    }
};

/// Whether a target's own binary is written in place of its text.
enum class sgl::emit::native_route : sgl::u8
{
    /// The text, for the compiler behind the target.
    text,
    /// The binary, or the text where the entry point holds something the native layer has no lowering for yet.
    prefer,
    /// The binary, or an error `unsupported` that names what is missing.
    require,
};

/// What an emit writes beside the text itself; the default writes no line table, no `#line` and no diagnostics.
struct sgl::emit::options
{
    /// Parallel to `checked_module::files`, as the check pass took them.
    cc::span<check::module_file const> files;
    /// Parallel to `files`: what a `#line` calls each one.
    cc::span<cc::string_view const> names;
    /// Writes `#line` where the target has it, and a comment where the inlined calls change (EMIT-182).
    bool write_directives = false;
    /// Writes each print into sg's debug buffer, and describes its site (EMIT-183).
    /// The tree must have been legalized to keep its prints and lower its asserts (LEGAL-64); otherwise a print is `unsupported`.
    bool diagnostics = false;
    /// What the device has; with `float_atomic_add`, vulkan and metal write the target's own float atomics.
    check::feature_set device_features;
    /// How each granted feature is written (EMIT-265); one it names no fill for is `native`.
    cc::span<check::feature_fill const> fills;
    /// How the core constructs are written (EMIT-265): `native` for both is what every compile wrote before fills.
    check::core_fills core;
    /// The tree was legalized to its emulated traces where the target has native ones (`text_request::software_trace`).
    bool software_trace = false;
    /// How `hlsl-vulkan` and `hlsl-dx12` are written: the text for DXC, or SPIR-V and DXIL from SGL's own layer (ir/ir.hh),
    /// with no compiler behind it.
    native_route native = native_route::text;
    /// Writes each operation's `.sgl` line into the native route's binary (EMIT-257); `files` must hold the sources.
    bool native_line_table = false;
    /// A mesh entry point as one half of its polyfill rather than as a mesh stage, and a pixel entry point as the one
    /// that follows the vertex half; every other entry ignores it.
    mesh_form mesh = mesh_form::native;
    /// The float guarantee the entry point keeps, already weighed against the target (EMIT-185).
    /// MSL states it as the text's math mode, which the Metal compiler honors over its own options (EMIT-262).
    check::float_level float_guarantee = check::float_level::fast;
};

/// Either the text or the reasons there is none.
struct sgl::emit::emitted_text
{
    /// Empty when `errors` is not; where `spirv` is written, the program it was written from, for a reader.
    cc::string text;
    /// The module, where `options::native` wrote one for vulkan; empty otherwise.
    cc::vector<u32> spirv;
    /// The container, where `options::native` wrote one for dx12; empty otherwise.
    cc::vector<byte> dxil;
    /// What kept a `prefer` from writing `spirv`, for a reader; empty otherwise.
    cc::string native_gap;
    /// Where DXIL's packer places what crosses the entry point's edge, wherever `options::native` asked for dx12.
    /// It stands even where `native_gap` does, since a signature needs no lowering of the body.
    cc::vector<ir::dxil::signature_element> signature;
    /// The name the text actually declares the entry point under, which is the source's unless this target reserves it.
    /// A caller compiling the text has to ask for THIS name, not the one it requested.
    cc::string entry_point;
    /// Every resource and block of constants the text declares, samplers included.
    /// A caller renames what the compiler reflects by it.
    cc::vector<bound_name> bound_names;
    /// A pixel entry point's render targets: how many, and the `@pixel struct` it returns; -1 and empty otherwise.
    i32 color_targets = -1;
    cc::string target_struct;
    /// Every constant block and buffer element the text declares, as it declares them.
    cc::vector<emitted_layout> layouts;
    /// A line of the function body per entry, in text order; empty unless the emit was given its `options`.
    cc::vector<line_origin> lines;
    /// Every print and assert the text writes into sg's debug buffer, which it declares when this is not empty (EMIT-183).
    cc::vector<debug_site> debug_sites;
    /// The slots a polyfilled mesh entry point's two halves agree on, which sg sizes its buffer and its draw by;
    /// empty for every other emit.
    cc::optional<check::mesh_polyfill_layout> mesh_layout;
    /// The records a polyfilled task stage's forms agree on, which sg sizes their buffer by; empty for every other emit.
    cc::optional<check::task_polyfill_layout> task_layout;
    /// What the output wrote of the core constructs natively, which a dx12 output needs Shader Model 6.8 for (EMIT-264):
    /// the draw's start as system values, and the preferred subgroup size as a range.
    bool reads_draw_start = false;
    bool states_wave_size_range = false;
    /// `hlsl-dx12` text: it states a payload's access qualifiers, which DXC takes below Shader Model 6.7 only with
    /// `-enable-payload-qualifiers` (EMIT-264).
    bool states_payload_qualifiers = false;
    /// The output talks to NVIDIA's driver through NVAPI's shader extensions at `u0, space13`: an `nvapi` fill wrote it
    /// (EMIT-265), and the text includes `nvHLSLExtns.h`.
    bool writes_nvapi = false;
    cc::vector<error> errors;

    [[nodiscard]] bool has_text() const { return errors.empty(); }

    [[nodiscard]] bool operator==(emitted_text const& rhs) const
    {
        return text == rhs.text && ast::impl::is_equal(spirv, rhs.spirv) && ast::impl::is_equal(dxil, rhs.dxil)
            && native_gap == rhs.native_gap && ast::impl::is_equal(signature, rhs.signature)
            && entry_point == rhs.entry_point && ast::impl::is_equal(bound_names, rhs.bound_names)
            && color_targets == rhs.color_targets && target_struct == rhs.target_struct
            && ast::impl::is_equal(layouts, rhs.layouts) && ast::impl::is_equal(lines, rhs.lines)
            && ast::impl::is_equal(debug_sites, rhs.debug_sites) && mesh_layout == rhs.mesh_layout
            && task_layout == rhs.task_layout && reads_draw_start == rhs.reads_draw_start
            && states_wave_size_range == rhs.states_wave_size_range
            && states_payload_qualifiers == rhs.states_payload_qualifiers && writes_nvapi == rhs.writes_nvapi
            && ast::impl::is_equal(errors, rhs.errors);
    }
};

namespace sgl::emit
{
/// Every target there is, in the order of the enum.
[[nodiscard]] cc::span<target const> all_targets();

/// The fills `t` offers for a granted feature `f`, most demanding first (EMIT-265): `native` everywhere, and `nvapi` too
/// for `shader_execution_reordering` and `opacity_micromap` on `hlsl-dx12`.
[[nodiscard]] cc::span<check::fill const> fills_of(target t, check::feature f);

/// The fills `t` offers for a core construct, most demanding first: on `hlsl-dx12`, `native` and the backport
/// (`root_constant` for the draw's start, `none` for the subgroup size); `native` alone elsewhere.
[[nodiscard]] cc::span<check::fill const> fills_of(target t, check::core_construct c);

/// `hlsl-dx12`, `hlsl-vulkan`, `wgsl`, `msl`.
[[nodiscard]] cc::string_view to_string(target t);

/// What a compile for `t` on a device with `device_features` stands for where an inlined body reads it (CHK-426).
[[nodiscard]] check::compile_target compile_target_of(target t, check::feature_set device_features = {});

/// The stable kebab-case name of a kind, e.g. "reserved-entry-point-name".
[[nodiscard]] cc::string_view to_string(error_kind kind);

/// The text of ONE entry point for one target: that entry point, and exactly the types and bindings it needs.
///
/// `entry_point` is a position in `m.entry_points`.
/// The check pass writes the structured form, so the tree goes through `check::legalize` first.
/// Stages compile apart, so nothing in the text depends on the text of another entry point.
/// An address is a position: member i of an edge struct is location i, counted over the members without `@position`.
///
/// Total: a module with errors, a position out of range and a construct no target carries yet are errors in the result.
/// No error depends on `t` but these, so an entry point written for one target is written for every other: `msl`
/// refuses a compute entry point and a group, which are both arguments of a Metal entry point and wait for a Metal compiler.
/// It refuses a geometry and a tessellation entry point too, since Metal has neither stage.
/// It refuses a ray-tracing stage that reaches a file sampler with a mip bias, or that two pipelines size the ray data
/// of differently.
/// And `wgsl` refuses an entry point needing a feature WebGPU never has, which is portability the shader opted out of.
/// Deterministic: equal arguments give equal text.
/// `sources` adds the line table, and the `#line` directives where it asks for them.
[[nodiscard]] emitted_text emit(check::checked_module const& m, isize entry_point, target t, options const& sources = {});

/// The same for a flat tree that stands in `m` without being one of its entry points, such as a legalized one.
/// `e` must be in the core form, or the result is the error `not-core`.
[[nodiscard]] emitted_text emit_entry_point(check::checked_module const& m,
                                            check::flat_entry_point const& e,
                                            target t,
                                            options const& sources = {});

/// One line per error, for tests and for reading by eye: `unsupported a print, which no target writes yet`.
[[nodiscard]] cc::string dump_errors(emitted_text const& e);
} // namespace sgl::emit
