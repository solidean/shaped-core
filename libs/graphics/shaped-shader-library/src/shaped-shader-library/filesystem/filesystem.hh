#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-shader-library/filesystem/watch.hh>
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

    /// Registers `sink` for changes under `prefix` — a path prefix, empty meaning this filesystem's whole
    /// root. The subscription unsubscribes on destruction.
    ///
    /// nullopt means "I cannot notify — poll me instead", which is where everything starts and where a
    /// platform without a watch backend stays.
    ///
    /// Relaxed on purpose: a notification is a HINT TO RESCAN, never a description of what changed. An
    /// implementation may coalesce, may fire spuriously, and may watch a whole directory when a single
    /// file was asked for — a sibling's change firing the sink is allowed, and filtering is optional.
    /// revision() remains the source of truth, which is what lets an inotify queue overflow or an editor
    /// that saves via write-temp-then-rename degrade to "fire the sink" rather than to a missed reload.
    [[nodiscard]] virtual cc::optional<watch_subscription> watch([[maybe_unused]] cc::string_view prefix,
                                                                 [[maybe_unused]] watch_sink sink) const
    {
        return cc::nullopt;
    }
};
} // namespace slib
