#include "diagnostic.hh"

#include <clean-core/common/assert.hh>

cc::string_view sgl::to_string(diagnostic_kind kind)
{
    switch (kind)
    {
    case diagnostic_kind::tab_in_indentation:
        return "tab-in-indentation";
    case diagnostic_kind::unknown_character:
        return "unknown-character";
    case diagnostic_kind::undelimited_string:
        return "undelimited-string";
    case diagnostic_kind::missing_string_end:
        return "missing-string-end";
    case diagnostic_kind::unknown_escape:
        return "unknown-escape";
    case diagnostic_kind::stray_dollar:
        return "stray-dollar";
    case diagnostic_kind::underindented_string_content:
        return "underindented-string-content";
    case diagnostic_kind::reserved_string_opener:
        return "reserved-string-opener";
    case diagnostic_kind::missing_closer:
        return "missing-closer";
    case diagnostic_kind::unmatched_closer:
        return "unmatched-closer";
    case diagnostic_kind::empty_block:
        return "empty-block";
    case diagnostic_kind::unattached_attribute:
        return "unattached-attribute";
    case diagnostic_kind::expected_expression:
        return "expected-expression";
    case diagnostic_kind::unexpected_token:
        return "unexpected-token";
    case diagnostic_kind::mixed_operators:
        return "mixed-operators";
    case diagnostic_kind::misplaced_not:
        return "misplaced-not";
    case diagnostic_kind::non_monotone_comparison:
        return "non-monotone-comparison";
    case diagnostic_kind::chained_range:
        return "chained-range";
    case diagnostic_kind::operator_needs_spaces:
        return "operator-needs-spaces";
    case diagnostic_kind::unknown_operator:
        return "unknown-operator";
    case diagnostic_kind::misplaced_attribute:
        return "misplaced-attribute";
    case diagnostic_kind::spaced_attribute_arguments:
        return "spaced-attribute-arguments";
    case diagnostic_kind::nested_continuation:
        return "nested-continuation";
    case diagnostic_kind::reserved_operator:
        return "reserved-operator";
    case diagnostic_kind::semicolon_in_parens:
        return "semicolon-in-parens";
    case diagnostic_kind::double_colon:
        return "double-colon";
    case diagnostic_kind::bare_range:
        return "bare-range";
    case diagnostic_kind::malformed_number:
        return "malformed-number";
    case diagnostic_kind::underscore_in_number:
        return "underscore-in-number";
    case diagnostic_kind::expected_declaration:
        return "expected-declaration";
    case diagnostic_kind::expected_member:
        return "expected-member";
    case diagnostic_kind::expected_case_arm:
        return "expected-case-arm";
    case diagnostic_kind::expected_name:
        return "expected-name";
    case diagnostic_kind::reserved_name:
        return "reserved-name";
    case diagnostic_kind::expected_pattern:
        return "expected-pattern";
    case diagnostic_kind::expected_parameter:
        return "expected-parameter";
    case diagnostic_kind::expected_body:
        return "expected-body";
    case diagnostic_kind::declaration_not_allowed_here:
        return "declaration-not-allowed-here";
    case diagnostic_kind::misplaced_module:
        return "misplaced-module";
    case diagnostic_kind::member_not_allowed_here:
        return "member-not-allowed-here";
    case diagnostic_kind::default_not_allowed_here:
        return "default-not-allowed-here";
    case diagnostic_kind::named_only_not_allowed_here:
        return "named-only-not-allowed-here";
    case diagnostic_kind::missing_parameter_list:
        return "missing-parameter-list";
    case diagnostic_kind::signature_out_of_order:
        return "signature-out-of-order";
    case diagnostic_kind::duplicate_signature_list:
        return "duplicate-signature-list";
    case diagnostic_kind::stray_else:
        return "stray-else";
    case diagnostic_kind::mixed_struct_type:
        return "mixed-struct-type";
    case diagnostic_kind::misplaced_splat:
        return "misplaced-splat";
    case diagnostic_kind::misplaced_attribute_on_expression:
        return "misplaced-attribute-on-expression";
    case diagnostic_kind::statement_in_expression:
        return "statement-in-expression";
    case diagnostic_kind::unexpected_keyword:
        return "unexpected-keyword";
    case diagnostic_kind::too_many_arguments:
        return "too-many-arguments";
    case diagnostic_kind::for_takes_name_in_range:
        return "for-takes-name-in-range";
    case diagnostic_kind::assert_takes_condition_and_message:
        return "assert-takes-condition-and-message";
    case diagnostic_kind::print_takes_one_message:
        return "print-takes-one-message";
    case diagnostic_kind::unsupported_syntax:
        return "unsupported-syntax";
    case diagnostic_kind::no_effect:
        return "no-effect";
    case diagnostic_kind::yield_in_function:
        return "yield-in-function";
    case diagnostic_kind::return_in_lambda:
        return "return-in-lambda";
    case diagnostic_kind::expected_object_element:
        return "expected-object-element";
    case diagnostic_kind::yield_in_loop:
        return "yield-in-loop";
    case diagnostic_kind::redundant_yield:
        return "redundant-yield";
    case diagnostic_kind::jump_without_target:
        return "jump-without-target";
    case diagnostic_kind::redundant_return:
        return "redundant-return";
    case diagnostic_kind::unsupported_yet:
        return "unsupported-yet";
    case diagnostic_kind::unknown_name:
        return "unknown-name";
    case diagnostic_kind::unknown_member:
        return "unknown-member";
    case diagnostic_kind::no_matching_overload:
        return "no-matching-overload";
    case diagnostic_kind::ambiguous_overload:
        return "ambiguous-overload";
    case diagnostic_kind::type_mismatch:
        return "type-mismatch";
    case diagnostic_kind::dependency_cycle:
        return "dependency-cycle";
    case diagnostic_kind::unknown_builtin:
        return "unknown-builtin";
    case diagnostic_kind::opaque_struct_needs_builtin:
        return "opaque-struct-needs-builtin";
    case diagnostic_kind::binding_not_listed:
        return "binding-not-listed";
    case diagnostic_kind::missing_field:
        return "missing-field";
    case diagnostic_kind::unknown_field:
        return "unknown-field";
    case diagnostic_kind::duplicate_field:
        return "duplicate-field";
    case diagnostic_kind::duplicate_declaration:
        return "duplicate-declaration";
    case diagnostic_kind::invalid_entry_point:
        return "invalid-entry-point";
    case diagnostic_kind::wrong_kind_of_name:
        return "wrong-kind-of-name";
    case diagnostic_kind::missing_type:
        return "missing-type";
    case diagnostic_kind::invalid_attribute_arguments:
        return "invalid-attribute-arguments";
    case diagnostic_kind::missing_return:
        return "missing-return";
    case diagnostic_kind::recursive_call:
        return "recursive-call";
    case diagnostic_kind::not_assignable:
        return "not-assignable";
    case diagnostic_kind::unreachable_code:
        return "unreachable-code";
    case diagnostic_kind::non_exhaustive_case:
        return "non-exhaustive-case";
    case diagnostic_kind::duplicate_case_pattern:
        return "duplicate-case-pattern";
    case diagnostic_kind::missing_value_in_arm:
        return "missing-value-in-arm";
    case diagnostic_kind::needs_feature:
        return "needs-feature";
    case diagnostic_kind::unknown_feature:
        return "unknown-feature";
    case diagnostic_kind::feature_not_declared:
        return "feature-not-declared";
    case diagnostic_kind::unused_require:
        return "unused-require";
    case diagnostic_kind::stage_not_allowed:
        return "stage-not-allowed";
    case diagnostic_kind::invalid_pipeline:
        return "invalid-pipeline";
    case diagnostic_kind::nesting_too_deep:
        return "nesting-too-deep";
    case diagnostic_kind::shadows_unshadowable:
        return "shadows-unshadowable";
    case diagnostic_kind::test_captures_runtime_value:
        return "test-captures-runtime-value";
    case diagnostic_kind::test_must_end_in_check:
        return "test-must-end-in-check";
    case diagnostic_kind::test_failed:
        return "test-failed";
    case diagnostic_kind::unmet_expectation:
        return "unmet-expectation";
    case diagnostic_kind::member_name_clash:
        return "member-name-clash";
    case diagnostic_kind::call_spelling:
        return "call-spelling";
    case diagnostic_kind::literal_not_representable:
        return "literal-not-representable";
    case diagnostic_kind::literal_conversion_result:
        return "literal-conversion-result";
    case diagnostic_kind::literal_needs_type:
        return "literal-needs-type";
    case diagnostic_kind::shift_out_of_range:
        return "shift-out-of-range";
    case diagnostic_kind::constant_without_value:
        return "constant-without-value";
    case diagnostic_kind::constant_not_representable:
        return "constant-not-representable";
    case diagnostic_kind::missing_sampler:
        return "missing-sampler";
    case diagnostic_kind::invalid_constant_argument:
        return "invalid-constant-argument";
    case diagnostic_kind::non_uniform_control_flow:
        return "non-uniform-control-flow";
    case diagnostic_kind::non_uniform_index:
        return "non-uniform-index";
    case diagnostic_kind::needless_nonuniform:
        return "needless-nonuniform";
    }
    CC_UNREACHABLE("unknown diagnostic_kind");
}

cc::string_view sgl::summary_of(diagnostic_kind kind)
{
    switch (kind)
    {
    case diagnostic_kind::tab_in_indentation:
        return "a tab in the indentation, which is spaces only";
    case diagnostic_kind::unknown_character:
        return "a character that starts no token of the language";
    case diagnostic_kind::undelimited_string:
        return "a one-line string that does not close on its line";
    case diagnostic_kind::missing_string_end:
        return "a multi-line string whose closing quote is not the line after its content";
    case diagnostic_kind::unknown_escape:
        return "a backslash followed by a character that is no escape";
    case diagnostic_kind::stray_dollar:
        return "a `$` in a string that starts no interpolation";
    case diagnostic_kind::underindented_string_content:
        return "a line of a multi-line string indented less than its content is";
    case diagnostic_kind::reserved_string_opener:
        return "a string opener that is reserved, such as `\"\"\"`";
    case diagnostic_kind::missing_closer:
        return "a bracket that is opened and never closed";
    case diagnostic_kind::unmatched_closer:
        return "a closing bracket with nothing open to close";
    case diagnostic_kind::empty_block:
        return "a block colon with no indented lines below it";
    case diagnostic_kind::unattached_attribute:
        return "an attribute with nothing after it to attach to";
    case diagnostic_kind::misplaced_attribute:
        return "an attribute in the middle of a line rather than at its start";
    case diagnostic_kind::spaced_attribute_arguments:
        return "a space between an attribute and its arguments, which then read as a separate value";
    case diagnostic_kind::nested_continuation:
        return "a continuation line that is continued again";
    case diagnostic_kind::expected_expression:
        return "a value is missing here";
    case diagnostic_kind::unexpected_token:
        return "a token that has no place here";
    case diagnostic_kind::mixed_operators:
        return "operators of different kinds in one run without parentheses, such as `and` with `or`";
    case diagnostic_kind::misplaced_not:
        return "a `not` on another operand than the last one of its run";
    case diagnostic_kind::non_monotone_comparison:
        return "a comparison chain that goes up and then down";
    case diagnostic_kind::chained_range:
        return "two range operators in one run";
    case diagnostic_kind::operator_needs_spaces:
        return "an infix operator without spaces on both sides";
    case diagnostic_kind::unknown_operator:
        return "an operator the language does not define";
    case diagnostic_kind::reserved_operator:
        return "an operator spelling kept free for later, such as `!` or `?`";
    case diagnostic_kind::semicolon_in_parens:
        return "a `;` inside parentheses, where elements are separated by commas";
    case diagnostic_kind::double_colon:
        return "a `::`, which is written `.` in this language";
    case diagnostic_kind::bare_range:
        return "a `..` that says nothing about whether its end is included";
    case diagnostic_kind::malformed_number:
        return "a symbol that starts with a digit and is no number";
    case diagnostic_kind::underscore_in_number:
        return "a `_` inside a number, which is written `'`";
    case diagnostic_kind::expected_declaration:
        return "a line here must be a declaration or a statement";
    case diagnostic_kind::expected_member:
        return "a line of this block is no member of it";
    case diagnostic_kind::expected_case_arm:
        return "a line of a `case` block that is no `pattern => result`";
    case diagnostic_kind::expected_name:
        return "a name is missing where this one stands";
    case diagnostic_kind::reserved_name:
        return "a reserved name, such as `void`, used as the name of something";
    case diagnostic_kind::expected_pattern:
        return "a `let` whose target is no name, no `_` and no list of patterns";
    case diagnostic_kind::expected_parameter:
        return "an element of a parameter list that is no parameter";
    case diagnostic_kind::expected_body:
        return "a construct that needs a body and has none";
    case diagnostic_kind::declaration_not_allowed_here:
        return "a declaration of a kind this place does not allow";
    case diagnostic_kind::misplaced_module:
        return "a `module` line that is not the first declaration of its file, or a second one";
    case diagnostic_kind::member_not_allowed_here:
        return "a member its owner does not allow, such as a method in a `binding`";
    case diagnostic_kind::default_not_allowed_here:
        return "a default value on a field whose owner allows none";
    case diagnostic_kind::named_only_not_allowed_here:
        return "a leading-dot name where only a plain name is allowed";
    case diagnostic_kind::missing_parameter_list:
        return "a signature without its `()`";
    case diagnostic_kind::signature_out_of_order:
        return "the lists of a signature in another order than `[…]`, `(…)`, `{…}`";
    case diagnostic_kind::duplicate_signature_list:
        return "a list of a signature that stands twice";
    case diagnostic_kind::stray_else:
        return "an `else` that pairs with no `if`";
    case diagnostic_kind::mixed_struct_type:
        return "curly braces holding both `name: type` and `name = value` elements";
    case diagnostic_kind::misplaced_splat:
        return "a splat that is not a whole element of a list";
    case diagnostic_kind::misplaced_attribute_on_expression:
        return "an attribute on an expression that is no type";
    case diagnostic_kind::statement_in_expression:
        return "a statement where a value is expected";
    case diagnostic_kind::unexpected_keyword:
        return "keywords that head nothing together, such as `mut` without `let`";
    case diagnostic_kind::too_many_arguments:
        return "a keyword that holds more expressions than it takes";
    case diagnostic_kind::for_takes_name_in_range:
        return "a `for` that is not `for name in expression`";
    case diagnostic_kind::assert_takes_condition_and_message:
        return "an `assert` that is not one condition, optionally with one message";
    case diagnostic_kind::print_takes_one_message:
        return "a `print` that is not exactly one message";
    case diagnostic_kind::unsupported_syntax:
        return "a spelling that is reserved and has no meaning yet";
    case diagnostic_kind::no_effect:
        return "a statement that computes a value and drops it";
    case diagnostic_kind::yield_in_function:
        return "a `yield` in a function body, which is left with `return`";
    case diagnostic_kind::return_in_lambda:
        return "a `return` in an arrow lambda's block, which hands its value on with `yield`";
    case diagnostic_kind::expected_object_element:
        return "an object element that is not `name`, `name = value` or a splat";
    case diagnostic_kind::yield_in_loop:
        return "a `yield` with a `loop` in between, which is left with `break value`";
    case diagnostic_kind::redundant_yield:
        return "a `yield` that the `=>` before it already says";
    case diagnostic_kind::jump_without_target:
        return "a jump with nothing around it to leave";
    case diagnostic_kind::redundant_return:
        return "a `return` as the whole body of an arrow lambda, where `x => x` says it";
    case diagnostic_kind::unsupported_yet:
        return "a construct the language has that this compiler does not carry yet";
    case diagnostic_kind::unknown_name:
        return "a name that nothing in scope declares";
    case diagnostic_kind::unknown_member:
        return "a member its type does not have";
    case diagnostic_kind::no_matching_overload:
        return "no function of this name takes these arguments";
    case diagnostic_kind::ambiguous_overload:
        return "two functions of this name take exactly these arguments";
    case diagnostic_kind::type_mismatch:
        return "a value of another type than the one expected here";
    case diagnostic_kind::dependency_cycle:
        return "a declaration that needs itself to be compiled";
    case diagnostic_kind::unknown_builtin:
        return "a `@builtin` declaration the compiler does not know";
    case diagnostic_kind::opaque_struct_needs_builtin:
        return "an opaque struct that is no `@builtin`";
    case diagnostic_kind::binding_not_listed:
        return "a binding member read in a function whose `{…}` list does not name the binding";
    case diagnostic_kind::missing_field:
        return "an object that leaves a field of its struct unnamed";
    case diagnostic_kind::unknown_field:
        return "an object that names a field its struct does not have";
    case diagnostic_kind::duplicate_field:
        return "an object that names one field twice";
    case diagnostic_kind::duplicate_declaration:
        return "a name declared twice in one scope";
    case diagnostic_kind::invalid_entry_point:
        return "an entry point that breaks a rule entry points follow";
    case diagnostic_kind::wrong_kind_of_name:
        return "a name that stands for something this position does not take";
    case diagnostic_kind::missing_type:
        return "a field, a binding member or a parameter without a type";
    case diagnostic_kind::invalid_attribute_arguments:
        return "an attribute with arguments it does not take";
    case diagnostic_kind::missing_return:
        return "a path through a function that ends without a `return`";
    case diagnostic_kind::recursive_call:
        return "a function that calls itself, directly or through others";
    case diagnostic_kind::not_assignable:
        return "an assignment to something that is not a mutable local or a member of one";
    case diagnostic_kind::unreachable_code:
        return "a statement after a jump, which never runs";
    case diagnostic_kind::non_exhaustive_case:
        return "a `case` that neither names every case of its enum nor has a `_` arm";
    case diagnostic_kind::duplicate_case_pattern:
        return "two arms of one `case` that name the same case";
    case diagnostic_kind::missing_value_in_arm:
        return "a `case` arm that neither produces a value nor exits";
    case diagnostic_kind::needs_feature:
        return "a form some backend lacks, used where no `require` grants its feature";
    case diagnostic_kind::stage_not_allowed:
        return "a function reached from an entry point of a stage its `@stages` leaves out";
    case diagnostic_kind::invalid_pipeline:
        return "a `pipeline` whose stages or settings do not fit together";
    case diagnostic_kind::shadows_unshadowable:
        return "a name that would hide a `@shadowable(false)` symbol";
    case diagnostic_kind::test_captures_runtime_value:
        return "a test that reads a value of the function it stands in";
    case diagnostic_kind::test_must_end_in_check:
        return "a test whose last code line is no check";
    case diagnostic_kind::test_failed:
        return "a test that ran and did not pass";
    case diagnostic_kind::unmet_expectation:
        return "an `@expect` whose diagnostic did not occur in its test";
    case diagnostic_kind::member_name_clash:
        return "two kinds of member under one name in one type";
    case diagnostic_kind::call_spelling:
        return "a property called like a function, or a function read like a property";
    case diagnostic_kind::literal_not_representable:
        return "a number literal that the expected type cannot hold exactly";
    case diagnostic_kind::literal_conversion_result:
        return "a literal converted by a function that returns another type";
    case diagnostic_kind::literal_needs_type:
        return "an operator over literals alone that only another type provides";
    case diagnostic_kind::shift_out_of_range:
        return "a shift by a constant count outside 0 to 31";
    case diagnostic_kind::constant_without_value:
        return "a call of constants that has no value, or an integer divided by a constant zero";
    case diagnostic_kind::constant_not_representable:
        return "a constant whose value its type cannot hold";
    case diagnostic_kind::missing_sampler:
        return "a texture sampled without a sampler, and without a `@sampler` to supply one";
    case diagnostic_kind::invalid_constant_argument:
        return "an argument taken only as a constant in a range, given something else";
    case diagnostic_kind::non_uniform_control_flow:
        return "a barrier or a derivative where not every invocation of its group arrives";
    case diagnostic_kind::non_uniform_index:
        return "an index into a binding array that may differ between invocations, without `nonuniform`";
    case diagnostic_kind::needless_nonuniform:
        return "a `nonuniform` mark on an index that is the same in every invocation";
    case diagnostic_kind::nesting_too_deep:
        return "an entry point that nests deeper than the compiler walks, once every call is inlined";
    case diagnostic_kind::unknown_feature:
        return "a `require` of a name that is no feature a shader can use";
    case diagnostic_kind::feature_not_declared:
        return "an entry point that uses a feature its file, its bindings and its body never `require`";
    case diagnostic_kind::unused_require:
        return "a `require` in a body that nothing there needed";
    }
    CC_UNREACHABLE("unknown diagnostic_kind");
}

sgl::severity sgl::default_severity_of(diagnostic_kind kind)
{
    switch (kind)
    {
    case diagnostic_kind::tab_in_indentation:
    case diagnostic_kind::unknown_character:
    case diagnostic_kind::undelimited_string:
    case diagnostic_kind::missing_string_end:
    case diagnostic_kind::unknown_escape:
    case diagnostic_kind::stray_dollar:
    case diagnostic_kind::underindented_string_content:
    case diagnostic_kind::reserved_string_opener:
    case diagnostic_kind::missing_closer:
    case diagnostic_kind::unmatched_closer:
    case diagnostic_kind::empty_block:
    case diagnostic_kind::unattached_attribute:
    case diagnostic_kind::expected_expression:
    case diagnostic_kind::unexpected_token:
    case diagnostic_kind::mixed_operators:
    case diagnostic_kind::misplaced_not:
    case diagnostic_kind::non_monotone_comparison:
    case diagnostic_kind::chained_range:
    case diagnostic_kind::operator_needs_spaces:
    case diagnostic_kind::unknown_operator:
    case diagnostic_kind::misplaced_attribute:
    case diagnostic_kind::nested_continuation:
    case diagnostic_kind::reserved_operator:
    case diagnostic_kind::semicolon_in_parens:
    case diagnostic_kind::double_colon:
    case diagnostic_kind::bare_range:
    case diagnostic_kind::malformed_number:
    case diagnostic_kind::underscore_in_number:
    case diagnostic_kind::expected_declaration:
    case diagnostic_kind::expected_member:
    case diagnostic_kind::expected_case_arm:
    case diagnostic_kind::expected_name:
    case diagnostic_kind::reserved_name:
    case diagnostic_kind::expected_pattern:
    case diagnostic_kind::expected_parameter:
    case diagnostic_kind::expected_body:
    case diagnostic_kind::declaration_not_allowed_here:
    case diagnostic_kind::misplaced_module:
    case diagnostic_kind::member_not_allowed_here:
    case diagnostic_kind::default_not_allowed_here:
    case diagnostic_kind::named_only_not_allowed_here:
    case diagnostic_kind::missing_parameter_list:
    case diagnostic_kind::signature_out_of_order:
    case diagnostic_kind::duplicate_signature_list:
    case diagnostic_kind::stray_else:
    case diagnostic_kind::mixed_struct_type:
    case diagnostic_kind::misplaced_splat:
    case diagnostic_kind::misplaced_attribute_on_expression:
    case diagnostic_kind::statement_in_expression:
    case diagnostic_kind::unexpected_keyword:
    case diagnostic_kind::too_many_arguments:
    case diagnostic_kind::for_takes_name_in_range:
    case diagnostic_kind::assert_takes_condition_and_message:
    case diagnostic_kind::print_takes_one_message:
    case diagnostic_kind::unsupported_syntax:
    case diagnostic_kind::yield_in_function:
    case diagnostic_kind::return_in_lambda:
    case diagnostic_kind::expected_object_element:
    case diagnostic_kind::yield_in_loop:
    case diagnostic_kind::jump_without_target:
    case diagnostic_kind::redundant_return:
    case diagnostic_kind::unsupported_yet:
    case diagnostic_kind::unknown_name:
    case diagnostic_kind::unknown_member:
    case diagnostic_kind::no_matching_overload:
    case diagnostic_kind::ambiguous_overload:
    case diagnostic_kind::type_mismatch:
    case diagnostic_kind::dependency_cycle:
    case diagnostic_kind::unknown_builtin:
    case diagnostic_kind::opaque_struct_needs_builtin:
    case diagnostic_kind::binding_not_listed:
    case diagnostic_kind::missing_field:
    case diagnostic_kind::unknown_field:
    case diagnostic_kind::duplicate_field:
    case diagnostic_kind::duplicate_declaration:
    case diagnostic_kind::invalid_entry_point:
    case diagnostic_kind::wrong_kind_of_name:
    case diagnostic_kind::missing_type:
    case diagnostic_kind::invalid_attribute_arguments:
    case diagnostic_kind::missing_return:
    case diagnostic_kind::recursive_call:
    case diagnostic_kind::not_assignable:
    case diagnostic_kind::non_exhaustive_case:
    case diagnostic_kind::duplicate_case_pattern:
    case diagnostic_kind::missing_value_in_arm:
    case diagnostic_kind::needs_feature:
    case diagnostic_kind::unknown_feature:
    case diagnostic_kind::feature_not_declared:
    case diagnostic_kind::stage_not_allowed:
    case diagnostic_kind::invalid_pipeline:
    case diagnostic_kind::nesting_too_deep:
    case diagnostic_kind::shadows_unshadowable:
    case diagnostic_kind::test_captures_runtime_value:
    case diagnostic_kind::test_must_end_in_check:
    case diagnostic_kind::test_failed:
    case diagnostic_kind::unmet_expectation:
    case diagnostic_kind::member_name_clash:
    case diagnostic_kind::call_spelling:
    case diagnostic_kind::literal_not_representable:
    case diagnostic_kind::literal_conversion_result:
    case diagnostic_kind::literal_needs_type:
    case diagnostic_kind::shift_out_of_range:
    case diagnostic_kind::constant_without_value:
    case diagnostic_kind::constant_not_representable:
    case diagnostic_kind::missing_sampler:
    case diagnostic_kind::invalid_constant_argument:
    case diagnostic_kind::non_uniform_control_flow:
    case diagnostic_kind::non_uniform_index:
        return severity::normal_error;
    case diagnostic_kind::spaced_attribute_arguments:
    case diagnostic_kind::needless_nonuniform:
    case diagnostic_kind::no_effect:
    case diagnostic_kind::redundant_yield:
    case diagnostic_kind::unreachable_code:
    case diagnostic_kind::unused_require:
        return severity::warning;
    }
    CC_UNREACHABLE("unknown diagnostic_kind");
}
