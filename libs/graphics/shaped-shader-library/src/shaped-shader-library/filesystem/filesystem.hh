#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-shader-library/fwd.hh>

namespace slib
{
/// Opaque content revision of a file. Any change to it means "reread me"; `none` means the file does
/// not exist. Deliberately not a timestamp — a real filesystem folds mtime into it, an in-memory one
/// just counts, so neither leaks its notion of time into the reload logic.
enum class file_revision : u64
{
    none = 0,
};

/// Read-only virtual filesystem — the only way slib reaches shader sources.
///
/// Paths are '/'-separated and relative to this filesystem's own root; an implementation normalizes
/// what it is given, and a path escaping the root resolves to nothing.
///
/// Reads must be safe from several threads at once: the reload watcher polls revision() while a
/// consumer may be in read_text().
class filesystem
{
public:
    virtual ~filesystem() = default;

    /// nullopt if the file does not exist.
    [[nodiscard]] virtual cc::optional<cc::string> read_text(cc::string_view path) const = 0;

    /// `none` if the file does not exist.
    [[nodiscard]] virtual file_revision revision(cc::string_view path) const = 0;

    /// Whether read_text would find something.
    [[nodiscard]] bool exists(cc::string_view path) const { return revision(path) != file_revision::none; }
};
} // namespace slib
