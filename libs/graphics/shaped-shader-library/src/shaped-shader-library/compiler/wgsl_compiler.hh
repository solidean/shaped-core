#pragma once

#include <clean-core/error/result.hh>
#include <shaped-shader-library/compiler/shader_compiler.hh>

#include <memory>

namespace slib
{
/// The compiler for a package authored in WGSL, which WebGPU consumes as source.
///
/// Nothing is compiled: the text is handed on as the shader's bytecode, and WebGPU compiles it when a pipeline is created.
/// What this adds is the reflection, read by `parse_wgsl_declarations`, and a shader that fails to reflect is an error on the async node.
/// It needs no toolchain, so it exists on every platform, WebAssembly included, and hot reload there is text in and text out.
[[nodiscard]] std::unique_ptr<shader_compiler> create_wgsl_compiler();
} // namespace slib
