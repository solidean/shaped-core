#pragma once

#include <clean-core/common/flags.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

/// What glob_matches does beyond a plain byte-for-byte match.
enum class cc::glob_option
{
    /// Fold ASCII case on both sides.
    /// What a caller comparing against a real file system wants on Windows, and what include spellings want everywhere.
    ignore_case,

    /// Run both sides through glob_normalize_path first.
    /// Leave it off only when the caller normalized already — a config comparing one path against many, say, which would otherwise renormalize it per candidate.
    normalize,
};

CC_FLAG_ENUM_INDEXED(cc, glob_option, u32);

namespace cc
{

/// The spelling every path a glob compares against is folded to, so a path and the pattern written for it are in the same alphabet.
///
/// Backslashes become forward slashes, and repeated or trailing slashes are collapsed away.
/// A drive is spelled natively and in lower case, so git-bash's `/c/x`, `C:\x` and `c:/x` all arrive as `c:/x`.
/// That rewrite is unconditional rather than Windows-only: these paths are only ever compared against each other, and a one-letter root directory is far rarer than a git-bash path.
///
/// This is a *lexical* normalization for matching, not a filesystem one: `.`, `..` and symlinks are left as written.
cc::string glob_normalize_path(cc::string_view path);

/// Match a path against a glob.
///
/// `?` is one character, `*` a run of them, and neither crosses a `/`.
/// `**` is the one that does: `src/**` is everything below `src/`, and `src/**/x.hh` also matches `src/x.hh`
/// — the `/` after a `**` is optional, which is what makes the zero-directory case work.
/// A pattern ending in `/` is shorthand for the subtree, so `tests/` and `tests/**` are the same glob.
///
/// Nothing is anchored for you: a pattern must describe the whole path, and matching a suffix takes a leading `**/`.
///
/// `options` is spelled at every call, `{}` included — which reads as an exact, case-sensitive match over two already-normalized paths.
/// Neither half of that is the obvious default, and a defaulted argument is exactly where a caller stops noticing which one it got.
bool glob_matches(cc::string_view pattern, cc::string_view path, cc::flags<glob_option> options);

} // namespace cc
