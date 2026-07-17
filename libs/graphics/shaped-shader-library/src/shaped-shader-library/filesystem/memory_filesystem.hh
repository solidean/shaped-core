#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-shader-library/filesystem/filesystem.hh>

namespace slib
{
/// An in-memory filesystem you write to directly. Every write bumps the file's revision, so a hot
/// reload is a write() rather than a sleep — which is how slib's reload tests stay deterministic.
///
/// Also the seam for shader sources that were never files: generated, downloaded, or authored in a UI.
class memory_filesystem final : public filesystem
{
public:
    [[nodiscard]] cc::optional<cc::string> read_text(cc::string_view path) const override;
    [[nodiscard]] file_revision revision(cc::string_view path) const override;

    /// Creates or replaces a file and bumps its revision. A path escaping the root asserts.
    void write(cc::string_view path, cc::string_view text);

    /// Removes a file; its revision drops back to `none`. Returns whether it existed.
    bool remove(cc::string_view path);

private:
    struct file
    {
        cc::string text;
        file_revision revision = file_revision::none;
    };

    struct state
    {
        cc::map<cc::string, file> files;
        // One counter for the whole filesystem, so a rewrite never reuses a revision a reader saw.
        u64 next_revision = 1;
    };

    // Guarded: the reload watcher polls revision() on its own thread while a writer replaces content.
    // Mutable so the const reads can take the lock.
    mutable cc::mutex<state> _state;
};
} // namespace slib
