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
