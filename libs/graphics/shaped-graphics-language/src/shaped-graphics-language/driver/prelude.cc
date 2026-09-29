#include "prelude.hh"

#include <clean-core/common/macros.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/uri.hh>
#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/builtins/registry.hh>

cc::span<sgl::prelude_file const> sgl::prelude_files()
{
    // Kept beside `builtins::default_registry()` for the same reason: generated once, immutable, and needed by every compile.
    static auto const builtins_text = builtins::default_registry().prelude_text();
    static prelude_file const files[] = {
        {.name = "builtins.sgl", .source = builtins_text},
        {.name = "core.sgl", .source = impl::embedded_core_prelude()},
    };
    return files;
}

cc::span<sgl::parsed_prelude_file const> sgl::parsed_prelude()
{
    static auto const parsed = []
    {
        auto out = cc::vector<parsed_prelude_file>();
        for (auto const& f : prelude_files())
        {
            auto file = parse(f.source);
            auto ast = ast::build(file);
            out.push_back({.file = cc::move(file), .ast = cc::move(ast)});
        }
        return out;
    }();
    return parsed;
}

namespace
{
/// `path` spelled one way: a `file://` uri decoded to its path, `/` as the separator, and lower case on Windows.
[[nodiscard]] cc::string normalized(cc::string_view path)
{
    auto out = cc::string(path);
    if (constexpr auto scheme = cc::string_view("file://"); path.starts_with(scheme))
    {
        auto decoded = cc::percent_decode(path.subview(scheme.size()));
        if (!decoded.has_value())
            return {};
        out = cc::move(decoded.value());
        // `/c:/x` is the Windows path `c:/x`
        if (out.size() >= 3 && out[0] == '/' && out[2] == ':')
            out = out.substring(1);
    }
    for (auto i = sgl::isize(0); i < out.size(); ++i)
    {
        if (out[i] == '\\')
            out[i] = '/';
#ifdef CC_OS_WINDOWS
        out[i] = cc::to_lower(out[i]);
#endif
    }
    return out;
}
} // namespace

sgl::i32 sgl::prelude_file_of(cc::string_view path)
{
    auto const given = normalized(path);
    auto const directory = normalized(impl::prelude_directory());
    auto const files = prelude_files();
    for (auto i = isize(0); i < files.size(); ++i)
        if (given == directory + "/" + normalized(files[i].name))
            return i32(i);
    return -1;
}
