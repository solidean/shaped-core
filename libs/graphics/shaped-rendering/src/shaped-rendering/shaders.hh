#pragma once

#include <shaped-rendering/fwd.hh>

namespace slib
{
class shader_library;
}

namespace sr
{
/// Adds the shader packages backing shaped-rendering's routines to `lib`.
/// Call it once at startup, before any routine runs and before `start_hot_reload`.
/// A routine acquires its shaders through the library, so without this it has nothing to compile:
///
///     slib::shader_library lib;
///     slib::add_available_compilers(lib);
///     sr::add_shader_packages(lib);
///     lib.start_hot_reload();
///
/// The packages are SGL and HLSL, so `lib` needs SGL compiler edges as well as DXC ones; `add_available_compilers` has both.
/// A caller never tracks which routine needs which package: this adds all of them.
void add_shader_packages(slib::shader_library& lib);
} // namespace sr
