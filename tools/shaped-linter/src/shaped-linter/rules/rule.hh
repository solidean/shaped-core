#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-linter/fwd.hh>
#include <shaped-linter/lex/source_buffer.hh>
#include <shaped-linter/lex/source_language.hh>
#include <shaped-linter/lex/source_span.hh>
#include <shaped-linter/lex/token_stream.hh>
#include <shaped-linter/parse/syntax_tree.hh>
#include <shaped-linter/prose/prose_view.hh>

namespace scl
{
enum class severity : u8
{
    note,
    warning,
    error,
};

/// Replace the bytes of `span` with `replacement`. The unit of an automatic fix.
/// An EMPTY span is an insertion at that offset — nothing is removed and `replacement` is spliced in.
struct text_edit
{
    source_span span;
    cc::string replacement;
};

/// A suggested fix: one or more edits applied together.
///
/// A fix must be safe to apply unattended: wherever the rule fires, applying it compiles and preserves
/// behavior.
/// A rewrite that is a judgement call belongs in a `hint` instead.
///
/// That contract is per-fix, which is what the second edit is usually for: a rewrite that only compiles
/// once the file also gains an include or a using-directive carries BOTH edits on every finding, rather
/// than putting the shared one on the first finding alone.
/// `collect_fix_edits` merges the byte-identical
/// copies, so the shared line still lands exactly once however many findings asked for it.
struct fix
{
    cc::vector<text_edit> edits;
};

/// A suggested rewrite that `--fix` deliberately does NOT apply — the nicer form that only a human can
/// sign off on, because it may fail to compile or (worse) change what the code means.
///
/// `message` is what to weigh, and is always printed.
/// `edits` are optional: a hint whose better form
/// cannot be spelled mechanically carries prose alone.
/// Nothing in the engine applies a hint's edits;
/// they exist so the reporter can show the exact replacement, and so a future `--apply-hints` could
/// offer them one at a time.
struct hint
{
    cc::string message;
    cc::vector<text_edit> edits;
};

/// One reported problem.
/// `rule_id` points at the reporting rule's stable literal (the greppable slug).
/// `span` is what to underline; the reporter renders the surrounding source around it, so a rule never formats anything itself.
/// `suggested_fix` is present when the rule can rewrite the code safely, `suggested_hint` when the better form needs a human.
/// The two are independent: a finding may carry both, and then the fix is what lands.
///
/// `primary_label` and `secondary` are opt-in and rarely needed — the message above the snippet usually says it all.
/// Reach for them when a finding is only intelligible as a relation between two places ("declared here" / "used here").
/// A secondary span may live in another file, which gets its own block.
struct finding
{
    cc::string_view rule_id;
    source_span span;
    cc::string message;
    severity sev = severity::warning;
    cc::optional<fix> suggested_fix;
    cc::optional<hint> suggested_hint;
    cc::string primary_label;
    cc::vector<label> secondary;
};

/// The layer a rule walks.
/// The engine builds only what some enabled rule asked for, so a cheap rule
/// stays cheap.
///
/// `tokens` and `syntax_tree` are the C++ pipeline, deepening in that order.
/// `prose` is a branch off it
/// rather than a step further along: it is the file's comments and body text, extracted per language, and
/// is the one layer that exists for markdown and Python too.
enum class rule_layer : u8
{
    tokens,
    syntax_tree,
    prose,
    // semantics — later
};

/// What a rule's `check` is handed: the source and what it was read as, the layers the engine built for
/// it, and the sink to report into.
///
/// Only the layers some enabled rule asked for are populated — `tree` is empty unless one wants the
/// syntax tree, `prose` unless one walks prose, and `tokens` holds nothing for markdown, which has no
/// lexer.
/// A rule reads the layer it declared and no other.
struct lint_context
{
    source_buffer const& source;
    source_language language = source_language::cpp;
    token_stream const& tokens;
    syntax_tree const& tree;
    prose_view const& prose;
    cc::vector<finding>& out;

    void report(finding f) { out.push_back(cc::move(f)); }
};

/// A rule: a stateless value in the registry.
/// `id` is the stable, greppable kebab-case slug (like a
/// clang-tidy check name); `rationale` is the mandatory `why` the reporter prints with every finding.
///
/// `languages` is the set of file kinds it fires on, and defaults to C++ alone — a rule never sees a file
/// it did not ask for, so a C++ rule is safe from ever meeting markdown.
struct rule
{
    cc::string_view id;
    cc::string_view rationale;
    rule_layer layer = rule_layer::tokens;
    u8 languages = k_cpp_only;
    severity default_severity = severity::warning;
    void (*check)(lint_context&) = nullptr;
};

inline bool applies_to(rule const& r, source_language l)
{
    return (r.languages & language_bit(l)) != 0;
}

/// Whether a set of rules needs the parse tree built.
inline bool any_needs_tree(cc::span<rule const> rules)
{
    for (auto const& r : rules)
        if (r.layer == rule_layer::syntax_tree)
            return true;
    return false;
}

/// Whether a set of rules needs the file's prose extracted.
inline bool any_needs_prose(cc::span<rule const> rules)
{
    for (auto const& r : rules)
        if (r.layer == rule_layer::prose)
            return true;
    return false;
}
} // namespace scl
