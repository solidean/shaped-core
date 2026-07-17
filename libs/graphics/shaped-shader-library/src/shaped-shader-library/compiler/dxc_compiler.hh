#pragma once

#include <clean-core/error/result.hh>
#include <shaped-shader-library/compiler/shader_compiler.hh>

#include <memory>

/// The HLSL -> DXIL compiler, backed by shaped-shader-compiler-dxc.
///
/// Only declared when slib was built with the DXC compiler available (SLIB_HAS_DXC): DXC is Windows-only
/// and fetched on demand, so the rest of slib — packages, mounts, reload — builds and is tested without
/// it. Guard a use of this header with `#if SLIB_HAS_DXC`, or just register whichever compilers your
/// build has.

#if SLIB_HAS_DXC

namespace slib
{
/// A compiler that turns a package's HLSL into DXIL for a dx12 context. Fails only on a broken DXC
/// install; a shader that does not compile is an error on the async node, not here.
///
/// Compiles are deduplicated and cached by content, so two shaders with identical flattened source (or
/// a reload that changes nothing) compile once. Compilation runs on the installed default async pool
/// (cc::install_default_async_pool); with none installed the node stays cold until something drives it.
[[nodiscard]] cc::result<std::unique_ptr<shader_compiler>> create_dxc_compiler();
} // namespace slib

#endif
