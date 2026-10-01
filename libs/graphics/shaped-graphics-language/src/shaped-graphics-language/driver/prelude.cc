#include "prelude.hh"

#include <clean-core/common/macros.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/uri.hh>
#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/check.hh>

cc::span<sgl::prelude_file const> sgl::prelude_files()
{
    // Kept beside `builtins::default_registry()` for the same reason: generated once, immutable, and needed by every compile.
    static auto const builtins_text = builtins::default_registry().prelude_text();
    static prelude_file const files[] = {
        {.name = "builtins.sgl", .source = builtins_text},
        {.name = "core.sgl", .source = impl::embedded_core_prelude()},
        {.name = "raytracing.sgl", .source = impl::embedded_raytracing_prelude()},
        {.name = "slug.sgl", .source = impl::embedded_slug_prelude()},
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

sgl::check::checked_prelude const* sgl::checked_prelude()
{
    static auto const checked = []
    {
        auto files = cc::vector<check::module_file>();
        for (auto const& f : parsed_prelude())
            files.push_back({.file = f.file, .ast = f.ast});
        return check::check_prelude(files, builtins::default_registry());
    }();
    return checked.has_value() ? &checked.value() : nullptr;
}

namespace
{
/// `path` spelled one way: a `file://` uri decoded to its path, `/` as the separator, no `.`, `..` or empty segment
/// that a lexical reading removes, and lower case on Windows.
/// A uri that does not decode stays as written, so it matches only itself.
[[nodiscard]] cc::string normalized(cc::string_view path)
{
    auto text = cc::string(path);
    if (constexpr auto scheme = cc::string_view("file://"); path.starts_with(scheme))
    {
        if (auto decoded = cc::percent_decode(path.subview(scheme.size())); decoded.has_value())
        {
            text = cc::move(decoded.value());
            // `/c:/x` is the Windows path `c:/x`
            if (text.size() >= 3 && text[0] == '/' && text[2] == ':')
                text = text.substring(1);
        }
    }
    for (auto i = sgl::isize(0); i < text.size(); ++i)
    {
        if (text[i] == '\\')
            text[i] = '/';
#ifdef CC_OS_WINDOWS
        text[i] = cc::to_lower(text[i]);
#endif
    }

    auto segments = cc::vector<cc::string_view>();
    auto rest = cc::string_view(text);
    while (!rest.empty())
    {
        auto const end = rest.find('/');
        auto const segment = end < 0 ? rest : rest.subview({.offset = 0, .size = end});
        rest = end < 0 ? cc::string_view() : rest.subview(end + 1);
        if (segment.empty() || segment == ".")
            continue;
        if (segment == ".." && !segments.empty() && segments.back() != "..")
            segments.remove_back();
        else
            segments.push_back(segment);
    }
    auto out = cc::string(text.starts_with("/") ? "/" : "");
    for (auto i = sgl::isize(0); i < segments.size(); ++i)
    {
        if (i > 0)
            out += '/';
        out += segments[i];
    }
    return out;
}
} // namespace

bool sgl::is_same_path(cc::string_view a, cc::string_view b)
{
    return normalized(a) == normalized(b);
}

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
