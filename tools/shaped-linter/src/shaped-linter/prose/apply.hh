#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-linter/fwd.hh>
#include <shaped-linter/lex/source_manager.hh>
#include <shaped-linter/prose/plan.hh>
#include <shaped-linter/prose/prose_view.hh>
#include <shaped-linter/rules/rule.hh>

namespace scl
{
struct apply_outcome;
struct apply_problems;
struct apply_report;
struct apply_settings;
struct file_prose_delta;
struct planned_rewrite;
} // namespace scl

/// One file's rewritten text, plus where the plan's new lines ended up in it.
///
/// `edited_lines` is in NEW-file coordinates and is what the prose re-check is scoped to: a violation the
/// plan did not write is not the plan's problem, which is the same non-ripple rule the `--changed-lines`
/// filter enforces for `--dirty-only`.
struct scl::planned_rewrite
{
    cc::string path;
    cc::string text;
    cc::vector<u32> edited_lines;
};

/// How `apply_prose_plan` runs.
struct scl::apply_settings
{
    /// Validate the whole plan and report, but write nothing.
    bool dry_run = false;

    /// Measure each file's prose before and after, filling `apply_report::prose`.
    bool stats = false;
};

/// One file's prose on either side of its rewrite.
struct scl::file_prose_delta
{
    cc::string path;
    prose_stats before;
    prose_stats after;
};

/// What `apply_prose_plan` did, or would have done under `--dry-run`.
struct scl::apply_report
{
    isize files_changed = 0;
    isize edits_applied = 0;

    /// In plan order, and empty unless `apply_settings::stats` asked for it.
    cc::vector<file_prose_delta> prose;
};

/// Everything wrong with a plan, collected rather than reported one at a time.
///
/// A plan is authored against many files at once, so failing on the first problem costs the author a
/// round trip per problem — every check therefore keeps going and appends here.
struct scl::apply_problems
{
    /// Structural failures: the target could not be read, a span ran past the end of it, or the edit
    /// changed code rather than prose.
    /// Each message already carries its own `path` or `path:line`.
    cc::vector<cc::string> errors;

    /// Prose rules firing on lines the plan WROTE.
    /// Spans point into the REWRITTEN text, so a reporter's carets land on the new prose rather than on
    /// what is still on disk — which means they resolve against `apply_outcome::sources`, not the files.
    cc::vector<finding> findings;

    [[nodiscard]] bool empty() const { return errors.empty() && findings.empty(); }
    [[nodiscard]] isize count() const { return errors.size() + findings.size(); }
};

/// The outcome of an apply: what was done, or everything that was wrong with the attempt.
///
/// `report` is meaningful only when `problems` is empty — a plan with any problem writes nothing at all.
/// `sources` owns the rewritten buffers `problems.findings` span into and must outlive them, which is why
/// it travels with them rather than being rebuilt by the caller.
struct scl::apply_outcome
{
    apply_report report;
    apply_problems problems;
    source_manager sources;
};

namespace scl
{

/// Rewrite one file's text per `file.edits` and report which new lines the plan wrote.
///
/// `original` must be the file the plan's line numbers were written against; a line number past its end is
/// an error rather than a clamp, since a stale plan is exactly what that looks like.
/// Pure — no filesystem, no validation.
/// `validate_rewrite` is the half that judges the result.
cc::result<planned_rewrite> build_rewrite(plan_file const& file, cc::string_view original);

/// Judge a rewrite, appending every problem it has to `out` rather than stopping at the first.
///
/// Two checks, in that order:
/// code is unchanged when the non-trivia token sequence is identical to `original`'s, which is what lets a
/// span cover a line that also holds code and still only permit its trailing comment to move;
/// and no prose rule may fire on a line the plan wrote.
/// Markdown has no lexer, so only the second check applies there — a markdown file is prose end to end and
/// is edited freely.
///
/// The rewritten text is registered in `sources` so the findings' spans resolve; pass the same manager for
/// every file of a plan.
void validate_rewrite(planned_rewrite const& rewritten,
                      cc::string_view original,
                      source_manager& sources,
                      apply_problems& out);

/// Build, validate and write every file in `plan`, resolving each path against `root`.
///
/// All-or-nothing: every file is rewritten and validated in memory first, so a plan with a problem in its
/// last file leaves the first one untouched on disk.
/// Both sides of the prose delta are therefore in hand before anything is written, which is why `--dry-run`
/// reports the same numbers a real run would.
///
/// A failing file does not stop the pass: every remaining file is still built and judged, so one run
/// reports every problem the plan has.
apply_outcome apply_prose_plan(prose_plan const& plan, cc::string_view root, apply_settings const& settings);
} // namespace scl
