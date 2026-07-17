#include <shaped-shader-library/filesystem/embedded_filesystem.hh>
#include <shaped-shader-library/filesystem/impl/path.hh>

namespace
{
// Embedded content cannot change, so one revision serves every file it has: not `none` (the file is
// there), and never bumped (there is nothing to reload).
constexpr slib::file_revision k_embedded_revision = slib::file_revision(1);
} // namespace

slib::embedded_file const* slib::embedded_filesystem::find(cc::string_view path) const
{
    auto const normalized = impl::normalize_path(path);
    if (!normalized.has_value())
        return nullptr;
    auto const& resolved = normalized.value();

    for (auto const& file : _files)
        if (file.path == resolved)
            return &file;
    return nullptr;
}

cc::optional<cc::string> slib::embedded_filesystem::read_text(cc::string_view path) const
{
    auto const* const file = find(path);
    if (file == nullptr)
        return cc::nullopt;
    return cc::string::create_copy_of(file->text);
}

slib::file_revision slib::embedded_filesystem::revision(cc::string_view path) const
{
    return find(path) == nullptr ? file_revision::none : k_embedded_revision;
}
