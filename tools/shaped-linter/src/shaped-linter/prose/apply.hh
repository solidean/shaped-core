#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-linter/fwd.hh>
#include <shaped-linter/prose/plan.hh>

namespace scl
{
/// One file's rewritten text, plus where the plan's new lines ended up in it.
///
/// `edited_lines` is in NEW-file coordinates and is what the prose re-check is scoped to: a violation the
/// plan did not write is not the plan's problem, which is the same non-ripple rule the `--changed-lines`
/// filter enforces for `--dirty-only`.
struct planned_rewrite
{
    cc::string path;
    cc::string text;
    cc::vector<u32> edited_lines;
};

/// What `apply_prose_plan` did, or would have done under `--dry-run`.
struct apply_report
{
    isize files_changed = 0;
    isize edits_applied = 0;
};

/// Rewrite one file's text per `file.edits` and report which new lines the plan wrote.
///
/// `original` must be the file the plan's line numbers were written against; a line number past its end is
/// an error rather than a clamp, since a stale plan is exactly what that looks like.
/// Pure — no filesystem, no validation.
/// `validate_rewrite` is the half that judges the result.
cc::result<planned_rewrite> build_rewrite(plan_file const& file, cc::string_view original);

/// Reject a rewrite that changed code, or that wrote prose the rules refuse.
///
/// Two checks, in that order:
/// code is unchanged when the non-trivia token sequence is identical to `original`'s, which is what lets a
/// span cover a line that also holds code and still only permit its trailing comment to move;
/// and no prose rule may fire on a line the plan wrote.
/// Markdown has no lexer, so only the second check applies there — a markdown file is prose end to end and
/// is edited freely.
cc::result<cc::unit> validate_rewrite(planned_rewrite const& rewritten, cc::string_view original);

/// Build, validate and write every file in `plan`, resolving each path against `root`.
///
/// All-or-nothing: every file is rewritten and validated in memory first, so a plan that fails on its last
/// file leaves the first one untouched on disk.
/// `dry_run` stops after validation and writes nothing.
cc::result<apply_report> apply_prose_plan(prose_plan const& plan, cc::string_view root, bool dry_run);
} // namespace scl
