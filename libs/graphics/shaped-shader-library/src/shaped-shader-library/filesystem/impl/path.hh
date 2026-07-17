#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

/// Virtual-path string helpers. Pure string manipulation — nothing here touches a filesystem, real or
/// virtual. slib paths are always '/'-separated and root-relative.

namespace slib::impl
{
/// Normalizes a virtual path: '\' becomes '/', '.' segments drop, '..' pops the preceding segment, and
/// leading / trailing / repeated slashes collapse. The result is root-relative — never leading '/'.
///
/// nullopt when the path escapes its root (a '..' with nothing left to pop). That is what confines a
/// mount to its own subtree, so it is the traversal guard rather than a tidiness pass.
[[nodiscard]] cc::optional<cc::string> normalize_path(cc::string_view path);

/// Normalized `base` (a directory) joined with `relative`. An empty base just normalizes `relative`.
[[nodiscard]] cc::optional<cc::string> join_path(cc::string_view base, cc::string_view relative);

/// The directory part of an already-normalized path; empty when it has none.
[[nodiscard]] cc::string_view parent_path(cc::string_view path);

/// True if `path` is `prefix` itself or lies beneath it. Segment-wise, so "foobar" is not under "foo".
/// An empty prefix is the root and contains everything. Both must be normalized.
[[nodiscard]] bool is_path_under(cc::string_view path, cc::string_view prefix);

/// `path` made relative to `prefix` (which must contain it, per is_path_under). Both must be normalized.
[[nodiscard]] cc::string_view relative_to(cc::string_view path, cc::string_view prefix);
} // namespace slib::impl
