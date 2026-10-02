#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics-language/driver/library_file.hh>
#include <shaped-shader-library/compiler/shader_compiler.hh>

namespace slib::impl
{
/// `modules` as the SGL compiler takes a library, viewing what `modules` views.
[[nodiscard]] inline cc::vector<sgl::library_file> sgl_library_of(cc::span<module_source const> modules)
{
    auto result = cc::vector<sgl::library_file>();
    for (auto const& m : modules)
        result.push_back({.name = m.path, .source = m.text});
    return result;
}
} // namespace slib::impl
