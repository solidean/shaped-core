#include <clean-core/common/assert.hh>
#include <shaped-shader-library/filesystem/impl/path.hh>
#include <shaped-shader-library/filesystem/memory_filesystem.hh>

cc::optional<cc::string> slib::memory_filesystem::read_text(cc::string_view path) const
{
    auto const normalized = impl::normalize_path(path);
    if (!normalized.has_value())
        return cc::nullopt;
    auto const& resolved = normalized.value();

    return _state.lock(
        [&](state const& s) -> cc::optional<cc::string>
        {
            auto const* const f = s.files.get_ptr(resolved);
            if (f == nullptr)
                return cc::nullopt;
            return f->text;
        });
}

slib::file_revision slib::memory_filesystem::revision(cc::string_view path) const
{
    auto const normalized = impl::normalize_path(path);
    if (!normalized.has_value())
        return file_revision::none;
    auto const& resolved = normalized.value();

    return _state.lock(
        [&](state const& s)
        {
            auto const* const f = s.files.get_ptr(resolved);
            return f == nullptr ? file_revision::none : f->revision;
        });
}

void slib::memory_filesystem::write(cc::string_view path, cc::string_view text)
{
    auto const normalized = impl::normalize_path(path);
    CC_ASSERT(normalized.has_value(), "path must not escape the filesystem root");
    auto const& resolved = normalized.value();

    _state.lock(
        [&](state& s)
        {
            auto& f = s.files[resolved];
            f.text = cc::string::create_copy_of(text);
            f.revision = file_revision(s.next_revision++);
        });
}

bool slib::memory_filesystem::remove(cc::string_view path)
{
    auto const normalized = impl::normalize_path(path);
    if (!normalized.has_value())
        return false;
    auto const& resolved = normalized.value();

    return _state.lock([&](state& s) { return s.files.erase(resolved); });
}
