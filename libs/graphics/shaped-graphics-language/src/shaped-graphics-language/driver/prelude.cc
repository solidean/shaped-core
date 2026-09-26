#include "prelude.hh"

#include <clean-core/string/string.hh>
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

sgl::i32 sgl::prelude_file_of(cc::string_view path)
{
    auto const is_separator = [](char c) { return c == '/' || c == '\\'; };
    auto const folder = cc::string_view("prelude");
    auto const files = prelude_files();
    for (auto i = isize(0); i < files.size(); ++i)
    {
        auto const name = files[i].name;
        if (!path.ends_with(name) || path.size() < name.size() + folder.size() + 1)
            continue;
        auto const before_name = path.subview({.offset = 0, .size = path.size() - name.size()});
        auto const in_folder = before_name.subview({.offset = 0, .size = before_name.size() - 1});
        if (!is_separator(before_name.back()) || !in_folder.ends_with(folder))
            continue;
        // `prelude` is a whole folder name, not the tail of `my_prelude`
        auto const outer = in_folder.size() - folder.size();
        if (outer == 0 || is_separator(in_folder[outer - 1]))
            return i32(i);
    }
    return -1;
}
