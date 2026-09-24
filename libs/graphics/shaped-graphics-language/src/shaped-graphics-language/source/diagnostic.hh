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
    /// No statement is exempt: a block hands a value on through `yield`, `return` or `break`, never by ending in it.
    no_effect,
    /// A `yield` whose nearest enclosing body is that of a `fun`, which is left with `return`.
    yield_in_function,
    /// A `return` in the block of an arrow lambda, which hands its value on with `yield`.
    return_in_lambda,
    /// An object element that is not `name`, `name = value` or `..splat`.
    expected_object_element,
    /// A `yield` with a `loop:` between it and its value block; a `loop` is left with `break value`.
    yield_in_loop,
    /// A `yield` that is the whole one-line body of an arm, an arrow lambda or a property; a warning.
    /// The `=>` already says where the value is, so the value is read as if the keyword were not there.
    redundant_yield,
    /// A jump with nothing to leave.
    /// A `yield` with neither a value block nor a `fun` around it, a `return` with no `fun` around it,
    /// and a `break` or a `continue` with no loop around it inside its function.
    jump_without_target,
    /// A `return` that is the whole one-line body of an arrow lambda: `x => return x` is written `x => x`.
    redundant_return,

    /// What follows is the check pass's: the AST was fine, and names and types are not.
    /// A construct the language has and this compiler does not carry yet; never a guess at its meaning.
    unsupported_yet,
    unknown_name,
    /// A struct without such a field, a binding without such a member, or a type that has no members at all.
    unknown_member,
    no_matching_overload,
    /// Two candidates take exactly these argument types.
    ambiguous_overload,
    type_mismatch,
    /// A symbol that needs itself to be compiled.
    dependency_cycle,
    /// A `@builtin` declaration whose name the compiler does not know, or knows as the other kind of declaration.
    unknown_builtin,
    opaque_struct_needs_builtin,
    /// A member of a binding, read in a function whose `{...}` list does not name that binding.
    binding_not_listed,
    /// An object that leaves a field of the struct it converts to unnamed.
    missing_field,
    /// An object that names a field the struct it converts to does not have.
    unknown_field,
    /// An object that names one field twice.
    duplicate_field,
    /// A name declared twice in one scope, unless every declaration of it is a function.
    duplicate_declaration,
    invalid_entry_point,
    /// A name that stands for something its position does not take: a function where a type is expected.
    wrong_kind_of_name,
    /// A field, a binding member or a parameter without a type.
    missing_type,
    /// An attribute the compiler knows, with arguments it does not take.
    invalid_attribute_arguments,
    /// A function that returns a value, with a path through its body that ends without a `return`.
    missing_return,
    /// A function that calls itself, directly or through others; every call is inlined, so none can.
    recursive_call,
    /// An assignment to what is no mutable local and no member of one.
    not_assignable,
    /// A statement after a jump in its list, which never runs; a warning.
    unreachable_code,
    /// A `case` that neither names every case of its enum nor carries a `_`.
    non_exhaustive_case,
    /// Two arms of one `case` that name one case of the enum.
    duplicate_case_pattern,
    /// An arm of a `case` that is a value and that neither produces one nor exits.
    missing_value_in_arm,
    /// A `pipeline` whose stages do not fit together, or a setting that names no field or has a value it cannot.
    invalid_pipeline,
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
