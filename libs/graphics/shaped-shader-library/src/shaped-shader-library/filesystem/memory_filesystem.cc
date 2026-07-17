#include <clean-core/common/assert.hh>
#include <shaped-shader-library/filesystem/impl/path.hh>
#include <shaped-shader-library/filesystem/impl/watch_registry.hh>
#include <shaped-shader-library/filesystem/memory_filesystem.hh>

// Out of line: _watches is a unique_ptr to a type the header only forward-declares.
slib::memory_filesystem::memory_filesystem() : _watches(std::make_unique<impl::watch_registry>())
{
}
slib::memory_filesystem::~memory_filesystem() = default;

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

    // Off the lock, and after the write: a sink runs arbitrary code, and it must be able to read the
    // revision it is being told about.
    _watches->fire_for(resolved);
}

bool slib::memory_filesystem::remove(cc::string_view path)
{
    auto const normalized = impl::normalize_path(path);
    if (!normalized.has_value())
        return false;
    auto const& resolved = normalized.value();

    auto const existed = _state.lock([&](state& s) { return s.files.erase(resolved); });
    if (existed)
        _watches->fire_for(resolved);
    return existed;
}

cc::optional<slib::watch_subscription> slib::memory_filesystem::watch(cc::string_view prefix, watch_sink sink) const
{
    auto normalized = impl::normalize_path(prefix);
    if (!normalized.has_value())
        return watch_subscription(); // nothing is reachable under a prefix that escapes the root

    return _watches->add(cc::move(normalized.value()), cc::move(sink));
}
