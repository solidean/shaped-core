#pragma once

#include <clean-core/error/result.hh>
#include <shaped-shader-library/compiler/shader_compiler.hh>

#include <memory>

/// The HLSL compilers backed by shaped-shader-compiler-dxc, one per bytecode format.
///
/// Only declared when slib was built with the DXC compiler available (SLIB_HAS_DXC): DXC is fetched on demand, so the rest of slib builds and is tested without it.
/// Guard a use of this header with `#if SLIB_HAS_DXC`, or just register whichever compilers your build has.
///
/// Registering both is the normal thing to do.
/// A shader_asset picks between them by asking the context what it accepts, so which one runs is a property of the backend a shader is acquired for rather than of the build.

#if SLIB_HAS_DXC

namespace slib
{
/// A compiler that turns a package's HLSL into DXIL for a dx12 context.
/// Fails only on a broken DXC install; a shader that does not compile is an error on the async node, not here.
///
/// Compiles are deduplicated and cached by content: identical flattened source with the same entry point, stage and options compiles once.
/// Compilation runs on the installed default async pool (cc::install_default_async_scheduler); with none installed the node stays cold until something drives it.
///
/// **Windows only in practice.** DXIL reflection reads a container beside the bytecode through the Windows SDK's
/// d3d12shader.h, which the Linux DXC release does not ship — so a compile here succeeds and then fails at reflection.
[[nodiscard]] cc::result<std::unique_ptr<shader_compiler>> create_dxc_compiler();

/// The same, emitting SPIR-V for a vulkan context.
///
/// Reflection is read out of the emitted module rather than a container, so this one works on every platform DXC does.
/// Bindings come back with `group_index` set from the SPIR-V descriptor set and `space` absent, which is what a vulkan
/// group layout needs — the HLSL register classes are mapped by `[[vk::binding]]` annotations in the shader source.
[[nodiscard]] cc::result<std::unique_ptr<shader_compiler>> create_dxc_spirv_compiler();
} // namespace slib

#endif
