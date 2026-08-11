#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-linter/fwd.hh>

namespace scl
{
struct plan_edit;
struct plan_file;
struct prose_plan;
} // namespace scl

/// One span of a prose plan: the lines to replace, and what to put there.
///
/// `first_line` / `last_line` are 1-based and inclusive, and cover whole lines — the format has no column
/// notion, so a line that also holds code is replaced whole and the code is protected by the token check
/// in `apply_prose_plan` instead.
/// An insertion carries `is_insertion` and only `first_line`: the new lines go BEFORE that line, and
/// nothing is removed.
/// `lines` is verbatim final text with the `| ` prefix already stripped — comment markers and indentation
/// included, because the applier deliberately infers neither.
/// Empty `lines` on a replacement is a deletion.
struct scl::plan_edit
{
    u32 first_line = 0;
    u32 last_line = 0;
    bool is_insertion = false;
    cc::vector<cc::string> lines;

    /// How many lines this edit removes — 0 for an insertion.
    u32 removed_line_count() const { return is_insertion ? 0 : last_line - first_line + 1; }
};

/// Every edit for one file, in ascending, non-overlapping line order (the parser enforces both).
struct scl::plan_file
{
    cc::string path;
    cc::vector<plan_edit> edits;
};

/// A parsed prose plan: what `shaped-linter prose apply` was asked to do.
struct scl::prose_plan
{
    cc::vector<plan_file> files;
};

namespace scl
{

/// Parse the plan text format.
///
/// A `## <path>` line opens a file section, `[a-b]` / `[a]` / `[+n]` opens a span, and every `| `-prefixed
/// line after it is one verbatim replacement line.
/// Blank lines separate; anything else is an error, so a typo fails the plan instead of being skipped.
///
/// Fails on a malformed header or span, a `| ` line before any span, a repeated file, a reversed range, a
/// zero line number, and on spans that are out of order or overlap.
/// Nothing here touches the filesystem, so line numbers are NOT yet known to be in range.
cc::result<prose_plan> parse_prose_plan(cc::string_view text);
} // namespace scl
