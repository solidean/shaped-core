#pragma once

#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/fwd.hh>
#include <shaped-graphics-language/source/source_span.hh>

/// How bad a diagnostic is for the *user's program*; parsing itself never fails.
enum class sgl::severity : sgl::u8
{
    /// The language forbids this, but there is one reasonable reading and the tree carries it.
    /// A development build continues past it; a production build treats it as an error.
    normal_error,
    /// No reasonable reading exists, so whatever depends on this node must not be trusted.
    fatal_error,
    /// Legal and almost certainly not what was meant.
    warning,
};

/// What went wrong, as a closed set.
/// Each kind has a stable kebab-case name, which is what a user sees, suppresses and searches for.
enum class sgl::diagnostic_kind : sgl::u8
{
    tab_in_indentation,
    unknown_character,
    undelimited_string,
    missing_string_end,
    unknown_escape,
    stray_dollar,
    underindented_string_content,
    reserved_string_opener,

    missing_closer,
    unmatched_closer,
    empty_block,
    unattached_attribute,
    misplaced_attribute,
    spaced_attribute_arguments,
    nested_continuation,

    expected_expression,
    unexpected_token,
    mixed_operators,
    /// A `not` anywhere but on the last operand of its run, where what it negates is a guess.
    misplaced_not,
    non_monotone_comparison,
    chained_range,
    operator_needs_spaces,
    unknown_operator,
    /// `!`, `?` and every postfix operator: spellings kept free for later.
    reserved_operator,
    semicolon_in_parens,
    double_colon,
    bare_range,
    malformed_number,
    underscore_in_number,
};

namespace sgl
{

/// The stable kebab-case name of a kind, e.g. "undelimited-string".
[[nodiscard]] cc::string_view to_string(diagnostic_kind kind);

[[nodiscard]] severity default_severity_of(diagnostic_kind kind);

} // namespace sgl

struct sgl::diagnostic
{
    diagnostic_kind kind;
    severity level;
    /// Never empty for a kind that names a character or a token; may be empty at end of file.
    source_span where;
};
