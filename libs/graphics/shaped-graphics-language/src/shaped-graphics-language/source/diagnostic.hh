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

    /// What follows is the AST pass's: the form tree was fine and the language has no reading for it.
    expected_declaration,
    expected_member,
    expected_case_arm,
    expected_name,
    expected_pattern,
    expected_parameter,
    expected_body,
    declaration_not_allowed_here,
    /// A second `module`, or one that is not the first declaration of its file.
    misplaced_module,
    /// The owner has no such member: a method in a `binding`, a field in an `enum`, a case in a `struct`.
    member_not_allowed_here,
    default_not_allowed_here,
    missing_parameter_list,
    signature_out_of_order,
    duplicate_signature_list,
    stray_else,
    mixed_struct_type,
    misplaced_splat,
    misplaced_attribute_on_expression,
    /// An assignment, a `let`, an `if` or a declaration where a value was expected.
    statement_in_expression,
    /// Keywords that head nothing together, such as `mut` without `let`.
    unexpected_keyword,
    /// A keyword form holding more expressions than it takes: `return a, b`, `continue x`.
    too_many_arguments,
    for_takes_name_in_range,
    assert_takes_condition_and_message,
    print_takes_one_message,
    /// A spelling that is reserved and has no meaning yet: `f(x){…}`, and an expression that owns a block.
    unsupported_syntax,
    /// An expression statement that is neither a call nor a jump; a warning, since it is legal and never meant.
    no_effect,
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

    constexpr bool operator==(diagnostic const&) const = default;
};
