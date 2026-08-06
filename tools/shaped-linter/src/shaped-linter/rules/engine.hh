#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-linter/fwd.hh>
#include <shaped-linter/lex/source_buffer.hh>
#include <shaped-linter/lex/source_manager.hh>
#include <shaped-linter/rules/registry.hh>
#include <shaped-linter/rules/rule.hh>

namespace scl
{
/// Lint one buffer: lex, parse (only if some rule needs the tree), run each rule, collect findings.
cc::vector<finding> run_rules(source_buffer const& buffer, cc::span<rule const> rules = all_rules());

/// Convenience for tests / snippets: lint in-memory text with a throwaway buffer.
/// The findings are self-contained (owned strings plus byte offsets), so they outlive the buffer.
cc::vector<finding> run_rules_on_text(cc::string_view source, cc::string_view path = "<memory>");

/// Apply `edits` to `original`, returning the rewritten text.
/// Edits are applied back-to-front (highest offset first), so earlier offsets stay valid; overlapping edits assert.
/// Only each edit's byte range within `original` is used — the file_id is ignored.
/// Pure, and the testable core of `apply_fixes`.
cc::string apply_edits(cc::string_view original, cc::span<text_edit const> edits);

/// Every `suggested_fix` edit across `findings`, byte-identical duplicates merged.
/// A finding's `suggested_hint` is deliberately not read.
///
/// The merge is what lets a fix add something shared by the whole file — an include, a using-directive.
/// Such a rule puts that edit on every finding it reports, so each fix stays safe to apply on its own.
/// Collected together they must still splice the line exactly once.
cc::vector<text_edit> collect_fix_edits(cc::span<finding const> findings);

/// Apply every finding's suggested edits back to their files in place, grouped per file.
/// Returns the number of files changed.
cc::result<isize> apply_fixes(source_manager const& sm, cc::span<finding const> findings);

/// Overwrite `path` with `content`. The one place the linter writes source back.
cc::result<cc::unit> write_file(cc::string_view path, cc::string_view content);
} // namespace scl
