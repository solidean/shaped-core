#pragma once

#include <shaped-viewer/fwd.hh>

namespace slib
{
struct shader_package;
}

namespace sv
{
/// The shader package backing shaped-viewer's render routines: the path tracer with module `tracer`, and the layout and depth-fill passes.
/// Register it with the shader library once at startup, before any routine runs — a routine acquires its shaders through the library, so without this it has nothing to compile:
///
///     slib::shader_library lib;
///     slib::add_available_compilers(lib);
///     lib.add_package(sv::shader_package());
///     lib.start_hot_reload();
///
/// Re-exposes the generated package, whose own header is private to shaped-viewer's build.
[[nodiscard]] slib::shader_package const& shader_package();
} // namespace sv
