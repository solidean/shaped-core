#pragma once

#include <clean-core/error/result.hh>
#include <clean-core/function/unique_function.hh>
#include <shaped-shader-library/fwd.hh>
#include <shaped-viewer/fwd.hh>

/// How a viewer gets hold of a shader library.
///
/// A provider only has to *create* one: it is called at most once per process, and `acquire_shader_library` is what makes that
/// happen only once — so a provider needs no static and no caching of its own.
///
/// It hands back a pointer rather than a value because a `slib::shader_library` is neither copyable nor movable, and because at
/// most one may exist per process: the generated package symbols it fills in are globals, and a second library would fight the
/// first over them.
namespace sv
{
using shader_library_provider = cc::unique_function<cc::result<slib::shader_library*>()>;

/// Sets the hook that decides which shader library viewers compile through.
///     sv::set_acquire_shader_library([] { return &my_library; });
/// Unset by default, and then `impl::acquire_default_shader_library` answers instead, which registers sv's and sr's packages plus
/// a DXC compiler where one exists.
/// Passing `{}` clears it again, which is how a test hands the default back.
///
/// The library must outlive every viewer using it.
void set_acquire_shader_library(shader_library_provider provider);

namespace impl
{
/// Creates the library used when no provider was set: sv's package, sr's (the blit the compositor drives), and DXC where it exists.
/// It creates unconditionally — `acquire_shader_library` is what makes that happen only once.
///
/// The library is deliberately never destroyed.
/// The package symbols an asset is reached through are process-wide globals that outlive any owner, so tearing the library down
/// at exit would leave them pointing at freed assets — which is why `shader_asset` only weakly references its library.
[[nodiscard]] cc::result<slib::shader_library*> acquire_default_shader_library();
} // namespace impl

/// The shader library every viewer compiles through: the caller's provider if they set one, otherwise the built-in default.
///
/// Created on the first call and shared by every caller after, which is what a *generated* shader needs: a material permutation is
/// compiled from the render path, which has no library of its own to reach for.
/// Not thread-safe, like the rest of viewer setup.
[[nodiscard]] cc::result<slib::shader_library*> acquire_shader_library();
} // namespace sv
