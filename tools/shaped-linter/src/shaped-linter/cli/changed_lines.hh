#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-linter/fwd.hh>

namespace scl
{
struct changed_lines;
} // namespace scl

/// The lines a run is allowed to report prose findings on, per file.
///
/// A prose finding sits on exactly one line, and that line either changed or it did not — so scoping a
/// dirty-only run to changed lines is exact rather than a heuristic.
/// It applies to prose rules ALONE: a code finding can be caused by a line the edit never touched, so
/// filtering those by line would hide real hits.
///
/// A file with no entry has no reportable lines.
/// That is what makes editing one section of a long file a terminating job: the file's other, older
/// violations are out of scope by construction and cannot ripple into the edit.
struct scl::changed_lines
{
    /// Whether any file was listed at all — an empty set means no filtering was requested.
    bool empty() const { return _files.empty(); }

    /// Whether `line` (1-based) is in scope for `path`. Separators are normalized, so `/` and `\` match.
    bool covers(cc::string_view path, u32 line) const;

private:
    struct line_range
    {
        u32 first = 0;
        u32 last = 0;
    };
    struct file_ranges
    {
        cc::string path; // separators normalized to '/'
        cc::vector<line_range> ranges;
    };
    cc::vector<file_ranges> _files;

    friend cc::result<changed_lines> parse_changed_lines(cc::string_view text);
};

namespace scl
{

/// Parse the `--changed-lines` spec: one `<path>:a-b,c-d` per line, blank lines ignored.
/// A single line is spelled `a-a`; the ranges of one path are unioned if it appears more than once.
cc::result<changed_lines> parse_changed_lines(cc::string_view text);
} // namespace scl
