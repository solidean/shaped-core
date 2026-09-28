#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
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
    /// `binding.member` for a resource, the binding's own name for a block of constants.
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
    cc::vector<error> errors;

    [[nodiscard]] bool has_text() const { return errors.empty(); }

    [[nodiscard]] bool operator==(emitted_text const& rhs) const
    {
        return text == rhs.text && entry_point == rhs.entry_point && ast::impl::is_equal(bound_names, rhs.bound_names)
            && color_targets == rhs.color_targets && target_struct == rhs.target_struct
            && ast::impl::is_equal(layouts, rhs.layouts) && ast::impl::is_equal(errors, rhs.errors);
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
/// And `wgsl` refuses an entry point needing a feature WebGPU never has, which is portability the shader opted out of.
/// Deterministic: equal arguments give equal text.
[[nodiscard]] emitted_text emit(check::checked_module const& m, isize entry_point, target t);

/// The same for a flat tree that stands in `m` without being one of its entry points, such as a legalized one.
/// `e` must be in the core form, or the result is the error `not-core`.
[[nodiscard]] emitted_text emit_entry_point(check::checked_module const& m, check::flat_entry_point const& e, target t);

/// One line per error, for tests and for reading by eye: `unsupported a print, which no target writes yet`.
[[nodiscard]] cc::string dump_errors(emitted_text const& e);
} // namespace sgl::emit
