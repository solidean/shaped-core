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
        return severity::normal_error;
    case diagnostic_kind::spaced_attribute_arguments:
        return severity::warning;
    }
    CC_UNREACHABLE("unknown diagnostic_kind");
}
