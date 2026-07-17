#pragma once

#include <clean-core/container/span.hh>
#include <shaped-shader-library/filesystem/filesystem.hh>

namespace slib
{
/// One file baked into the binary by the shader-package generator. Paths are normalized at generation.
struct embedded_file
{
    cc::string_view path;
    cc::string_view text;
};

/// A filesystem over files compiled into the binary — what a shipped build reads when there is no
/// source tree to mount over it. Content is fixed, so every file sits at one constant revision and
/// nothing ever reloads.
///
/// Non-owning: `files` and the text it points at must outlive the filesystem. Generated package data
/// has static storage, which is the intended source.
class embedded_filesystem final : public filesystem
{
public:
    explicit embedded_filesystem(cc::span<embedded_file const> files) : _files(files) {}

    [[nodiscard]] cc::optional<cc::string> read_text(cc::string_view path) const override;
    [[nodiscard]] file_revision revision(cc::string_view path) const override;

    /// A subscription that never fires — the whole truth for content that cannot change, and why a
    /// shipped build's watcher does nothing at all. Deliberately not nullopt: "I will never notify" is a
    /// different claim from "I cannot notify, poll me".
    [[nodiscard]] cc::optional<watch_subscription> watch([[maybe_unused]] cc::string_view prefix,
                                                         [[maybe_unused]] watch_sink sink) const override
    {
        return watch_subscription();
    }

private:
    // Linear scan: a package is tens of files, and lookups happen at compile time, not per frame.
    [[nodiscard]] embedded_file const* find(cc::string_view path) const;

    cc::span<embedded_file const> _files;
};
} // namespace slib
