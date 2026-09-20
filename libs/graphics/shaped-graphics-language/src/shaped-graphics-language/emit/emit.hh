#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/check/checked_module.hh>

/// The text a graphics API compiles, written from one flat entry point of a checked module.
///
/// A target is a text format together with the addressing rules of the backend that reads it.
/// So the two HLSL targets are two outputs: each carries its final addresses, and no later pass numbers anything.
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
    /// The entry point's name is a reserved word of some target, and an entry point is never renamed.
    reserved_entry_point_name,
    /// A construct the emitters do not carry yet; never a guess at its address or its meaning.
    unsupported,
    /// A vertex input member whose semantic would start with `SV_`, which dx12 reads as a system value.
    system_value_semantic,
    /// The members of an inline binding land on different offsets in HLSL, in WGSL and in MSL.
    layout_mismatch,
    /// A literal that is infinite or not a number, which no target can spell.
    non_finite_literal,
    /// A flat tree the check pass does not produce: an unfilled node, or a call of something that is no builtin function.
    malformed_tree,
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

/// Either the text or the reasons there is none.
struct sgl::emit::emitted_text
{
    /// Empty when `errors` is not.
    cc::string text;
    cc::vector<error> errors;

    [[nodiscard]] bool has_text() const { return errors.empty(); }

    [[nodiscard]] bool operator==(emitted_text const& rhs) const
    {
        return text == rhs.text && ast::impl::is_equal(errors, rhs.errors);
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
/// Stages compile apart, so nothing in the text depends on the text of another entry point.
/// An address is a position: member i of an edge struct is location i, counted over the members without `@position`.
///
/// Total: a module with errors, a position out of range and a construct no target carries yet are errors in the result.
/// No error depends on `t`, so an entry point that is written for one target is written for all of them.
/// Deterministic: equal arguments give equal text.
[[nodiscard]] emitted_text emit(check::checked_module const& m, isize entry_point, target t);

/// One line per error, for tests and for reading by eye: `unsupported a binding that is not @inline: 'scene'`.
[[nodiscard]] cc::string dump_errors(emitted_text const& e);
} // namespace sgl::emit
