#include "module_dirs.hh"

#include "files.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/string/format.hh>

// TEMPORARY: clean-core has no directory listing yet, as libs/graphics/shaped-graphics-language/docs/TODO.md records.
// Once it has one, this file lists through it and drops <filesystem>.
#include <filesystem>

using namespace cc::primitive_defines;

cc::result<sgl_tool::module_library, cc::string> sgl_tool::read_module_dirs(cc::span<cc::string const> dirs)
{
    namespace fs = std::filesystem;
    auto result = module_library();
    for (auto const& dir : dirs)
    {
        auto ec = std::error_code();
        auto found = cc::vector<cc::string>();
        for (auto const& e : fs::directory_iterator(fs::path(cc::string(dir).c_str_materialize()), ec))
            if (e.is_regular_file() && e.path().extension() == ".sgl")
                found.push_back(cc::format("{}/{}", dir, e.path().filename().string().c_str()));
        if (ec)
            return cc::error(cc::format("cannot list module directory {}: {}", dir, ec.message().c_str()));
        // in a stable order, so a diagnostic about the library never depends on the filesystem's
        cc::sort(found);
        for (auto& path : found)
        {
            auto source = read_file(path);
            if (source.has_error())
                return cc::error(cc::format("cannot read {}: {}", path, source.error()));
            result.names.push_back(cc::move(path));
            result.sources.push_back(cc::move(source.value()));
        }
    }
    for (auto i = isize(0); i < result.names.size(); ++i)
        result.files.push_back({.name = result.names[i], .source = result.sources[i]});
    return result;
}
