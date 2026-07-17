#include <clean-core/common/hash.hh>
#include <shaped-shader-library/filesystem/impl/path.hh>
#include <shaped-shader-library/filesystem/impl/watch_backend.hh>
#include <shaped-shader-library/filesystem/real_filesystem.hh>

// This file and its watch backends are the only part of slib allowed to reach the real disk. <filesystem>
// is not a blessed clean-core header (see libs/base/clean-core/docs/blessed-stdlib-headers.md), and it stays
// contained here: every other part of slib addresses shader sources through a mounted slib::filesystem.
// Keeping the dependency to one adaptor is also what makes the eventual move to a cc:: virtual filesystem a
// local change.
#include <filesystem>
#include <fstream>

slib::real_filesystem::real_filesystem(cc::string root_dir) : _root_dir(cc::move(root_dir))
{
}

// Out of line: _watch_state holds a unique_ptr to a type the header only forward-declares.
slib::real_filesystem::~real_filesystem() = default;

cc::optional<cc::string> slib::real_filesystem::to_native_path(cc::string_view path) const
{
    // normalize_path is the traversal guard: it rejects anything climbing past the root, so what we
    // append below can only ever land inside _root_dir.
    auto const normalized = impl::normalize_path(path);
    if (!normalized.has_value())
        return cc::nullopt;
    auto const& resolved = normalized.value();

    auto native = cc::string::create_copy_of(_root_dir);
    if (!resolved.empty())
    {
        native.push_back('/');
        native.append(resolved);
    }
    return native;
}

cc::optional<cc::string> slib::real_filesystem::read_text(cc::string_view path) const
{
    auto native = to_native_path(path); // non-const: c_str_materialize appends the terminator
    if (!native.has_value())
        return cc::nullopt;

    std::ifstream in(native.value().c_str_materialize(), std::ios::binary);
    if (!in.is_open())
        return cc::nullopt;

    cc::string text;
    char buffer[4096];
    while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0)
        text.append(cc::string_view(buffer, cc::isize(in.gcount())));

    if (in.bad())
        return cc::nullopt;
    return text;
}

slib::file_revision slib::real_filesystem::revision(cc::string_view path) const
{
    auto native = to_native_path(path); // non-const: c_str_materialize appends the terminator
    if (!native.has_value())
        return file_revision::none;

    std::error_code ec;
    auto const fs_path = std::filesystem::path(native.value().c_str_materialize());
    if (!std::filesystem::is_regular_file(fs_path, ec) || ec)
        return file_revision::none;

    auto const written = std::filesystem::last_write_time(fs_path, ec);
    if (ec)
        return file_revision::none;

    // Size joins mtime because a coarse filesystem clock can leave two quick edits sharing a timestamp.
    auto const size = std::filesystem::file_size(fs_path, ec);
    if (ec)
        return file_revision::none;

    auto const mixed = cc::make_hash_finalized(u64(written.time_since_epoch().count()), u64(size));
    return file_revision(mixed == 0 ? 1 : mixed); // never collide with `none`
}

cc::optional<slib::watch_subscription> slib::real_filesystem::watch(cc::string_view prefix, watch_sink sink) const
{
    auto native = to_native_path(prefix);
    if (!native.has_value())
        return watch_subscription(); // nothing is reachable under a prefix that escapes the root

    // A prefix naming an existing file is watched as the directory holding it: that is what the OS offers,
    // and the relaxed contract is what lets us take it — a sibling's change then fires the sink, and the
    // rescan it asks for finds nothing to do. A path that does not exist is left exactly as it is, so the
    // backend can fail on it and send the caller back to polling.
    if (std::error_code ec;
        std::filesystem::is_regular_file(std::filesystem::path(native.value().c_str_materialize()), ec) && !ec)
        native = cc::string::create_copy_of(impl::parent_path(native.value()));

    auto* const backend = _watch_state.lock(
        [](watch_state& s)
        {
            // Once: a platform with no backend has none the second time either, and building one costs a
            // thread and a completion port.
            if (!s.created)
            {
                s.backend = impl::create_watch_backend();
                s.created = true;
            }
            return s.backend.get();
        });

    if (backend == nullptr)
        return cc::nullopt; // no backend on this platform, or no threads to run one on: poll me instead

    return backend->watch_dir(native.value(), cc::move(sink));
}
