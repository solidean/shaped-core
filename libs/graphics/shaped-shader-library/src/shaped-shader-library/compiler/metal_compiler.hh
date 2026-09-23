#pragma once

#include <shaped-shader-library/compiler/shader_compiler.hh>

#include <memory>

namespace slib
{
/// The compiler for a package whose shaders reach metal: MSL in, an `sg::compiled_shader` out.
///
/// It is the edge `shaped-shader-compiler-msl` exists for, and the one an SGL package rides to metal —
/// `create_sgl_compiler(create_metal_compiler())` emits MSL and compiles it, since sgl's `metal_lib` target is MSL.
///
/// The artifact is a metallib where Apple's Metal toolchain is installed and MSL source where it is not, and
/// `target_format()` reports `metal_lib` either way because that is the edge slib resolves on — what a compile
/// actually produced is on the shader.
/// Exists only on Apple targets, which is what `SLIB_HAS_METAL` says.
[[nodiscard]] std::unique_ptr<shader_compiler> create_metal_compiler();
} // namespace slib
