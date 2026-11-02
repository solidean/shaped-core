#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/check/check.hh>
#include <shaped-graphics-language/check/checked_module.hh>

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

/// A line of an SGL source: a position in `checked_module::files`, and the line there, counted from 1.
struct sgl::emit::source_line
{
    /// -1 for no line at all.
    i32 file = -1;
    i32 line = 0;

    constexpr bool operator==(source_line const&) const = default;
};

/// Where one line of the text came from (EMIT-181).
struct sgl::emit::line_origin
{
    /// The line of the text, counted from 1.
    i32 emitted = 0;
    /// The innermost source line outside the prelude: a prelude function's body maps to the line that called it.
    source_line at;
    /// The calls `at` was inlined through, innermost first, the prelude's left out; empty for the entry point's own body.
    cc::vector<source_line> inlined_from;

    bool operator==(line_origin const& rhs) const
    {
        return emitted == rhs.emitted && at == rhs.at && ast::impl::is_equal(inlined_from, rhs.inlined_from);
    }
};

/// One value a debug record holds: its scalar family and its lanes, each lane one 32-bit word.
struct sgl::emit::debug_value
{
    /// `float`, `half`, `int`, `uint` or `bool`; an enum is `int`, and `type` names it.
    cc::string family;
    i32 lanes = 1;
    /// The value's type as the program names it: `float3`, `material_kind`.
    cc::string type;

    bool operator==(debug_value const&) const = default;
};

/// One print or assert the text writes into sg's debug buffer (EMIT-183).
/// Its record is `id`, the record's word count, then each value's lanes in order, one word each.
struct sgl::emit::debug_site
{
    /// What the record starts with, stable for one source line and message wherever the text is written.
    u32 id = 0;
    /// An assert's, which writes only where its condition was false.
    bool is_assert = false;
    /// The message around its values, one piece more than `values`; empty for a print of one bare value.
    cc::vector<cc::string> pieces;
    cc::vector<debug_value> values;
    /// Where the print or assert stands; empty without the source files (EMIT-181).
    line_origin origin;

    bool operator==(debug_site const& rhs) const
    {
        return id == rhs.id && is_assert == rhs.is_assert && ast::impl::is_equal(pieces, rhs.pieces)
            && ast::impl::is_equal(values, rhs.values) && origin == rhs.origin;
    }
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
};

/// Either the text or the reasons there is none.
struct sgl::emit::emitted_text
{
    /// Empty when `errors` is not.
    cc::string text;
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
    cc::vector<error> errors;

    [[nodiscard]] bool has_text() const { return errors.empty(); }

    [[nodiscard]] bool operator==(emitted_text const& rhs) const
    {
        return text == rhs.text && entry_point == rhs.entry_point && ast::impl::is_equal(bound_names, rhs.bound_names)
            && color_targets == rhs.color_targets && target_struct == rhs.target_struct
            && ast::impl::is_equal(layouts, rhs.layouts) && ast::impl::is_equal(lines, rhs.lines)
            && ast::impl::is_equal(debug_sites, rhs.debug_sites) && ast::impl::is_equal(errors, rhs.errors);
    }
};

namespace sgl::emit
{
/// Every target there is, in the order of the enum.
[[nodiscard]] cc::span<target const> all_targets();

/// `hlsl-dx12`, `hlsl-vulkan`, `wgsl`, `msl`.
[[nodiscard]] cc::string_view to_string(target t);

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
