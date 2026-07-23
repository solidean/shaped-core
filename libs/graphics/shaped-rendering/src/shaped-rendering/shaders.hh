#pragma once

#include <shaped-rendering/fwd.hh>

namespace slib
{
struct shader_package;
}

namespace sr
{
/// The shader package backing shaped-rendering's routines.
/// Register it with the shader library once at startup, before any routine runs —
/// a routine acquires its shaders through the library, so without this it has nothing to compile:
///
///     slib::shader_library lib;
///     lib.add_compiler(slib::create_dxc_compiler().value());
///     lib.add_package(sr::shader_package());
///     lib.start_hot_reload();
///
/// One package covers the whole library;
/// routines added later contribute their entry points to it, so a caller never has to track which routine needs which package.
///
/// This re-exposes the generated package, whose own header is private to shaped-rendering's build.
[[nodiscard]] slib::shader_package const& shader_package();
} // namespace sr
