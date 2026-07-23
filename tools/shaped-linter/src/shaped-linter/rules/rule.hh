#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-linter/fwd.hh>
#include <shaped-linter/lex/source_buffer.hh>
#include <shaped-linter/lex/source_span.hh>
#include <shaped-linter/lex/token_stream.hh>
#include <shaped-linter/parse/syntax_tree.hh>

namespace scl
{
enum class severity : u8
{
    note,
    warning,
    error,
};

/// Replace the bytes of `span` with `replacement`. The unit of an automatic fix.
struct text_edit
{
    source_span span;
    cc::string replacement;
};

/// A suggested fix: one or more edits applied together. A single edit today; the vector future-proofs
/// multi-edit rewrites.
struct fix
{
    cc::vector<text_edit> edits;
};

/// One reported problem. `rule_id` points at the reporting rule's stable literal (the greppable slug).
/// `span` is what to underline. `suggested_fix` is present when the rule knows how to rewrite the code.
struct finding
{
    cc::string_view rule_id;
    source_span span;
    cc::string message;
    severity sev = severity::warning;
    cc::optional<fix> suggested_fix;
};

/// The layers a rule can walk. A rule declares the highest it needs; the engine builds the parse tree
/// only when some enabled rule needs `syntax_tree`.
enum class rule_layer : u8
{
    tokens,
    syntax_tree,
    // semantics — later
};

/// What a rule's `check` is handed: the source, its tokens, its parse tree (empty if no enabled rule
/// needed it), and the sink to report into.
struct lint_context
{
    source_buffer const& source;
    token_stream const& tokens;
    syntax_tree const& tree;
    cc::vector<finding>& out;

    void report(finding f) { out.push_back(cc::move(f)); }
};

/// A rule: a stateless value in the registry. `id` is the stable, greppable kebab-case slug (like a
/// clang-tidy check name); `rationale` is the mandatory `why` the reporter prints with every finding.
struct rule
{
    cc::string_view id;
    cc::string_view rationale;
    rule_layer layer = rule_layer::tokens;
    severity default_severity = severity::warning;
    void (*check)(lint_context&) = nullptr;
};

/// Whether a set of rules needs the parse tree built.
inline bool any_needs_tree(cc::span<rule const> rules)
{
    for (auto const& r : rules)
        if (r.layer == rule_layer::syntax_tree)
            return true;
    return false;
}
} // namespace scl
