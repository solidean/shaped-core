#pragma once

#include <shaped-shader-library/compiler/shader_compiler.hh>

#include <memory>

namespace slib
{
/// The compiler for a package authored in SGL: one edge `sgl -> inner->target_format()` over the compiler that builds that format.
///
/// `preprocess` runs SGL's whole pipeline and hands back the text of the target `inner` compiles:
/// HLSL for dx12 over a dxil compiler, HLSL for vulkan over a spirv one, WGSL over the wgsl one, MSL over a metal_lib one.
/// The metal_lib edge is `create_metal_compiler()`, so an SGL package compiles for metal like any other target.
/// So the flattened source a `shader_asset` keeps and a compiler's cache hashes IS the emitted text, and `compile` is `inner`'s.
/// Each compile hands `inner` what that text needs, `-enable-16bit-types` for DXC and MSL 3.2 for a metallib, so `inner` is built with no setting.
/// Reflection is `inner`'s too: whatever it reads out of that text is what the shader reports.
///
/// `inner` must not be null, and its format must be dxil, spirv, wgsl or metal_lib.
/// Its `preprocess` is never called: SGL has no `#include`, and the emitted text has none either.
///
/// An SGL error is a `preprocess` error, so it rides the failure channel a DXC error does, one line per diagnostic:
/// `cube_shaders/cube.sgl:12:5: error: unknown-name: foo`.
/// SGL has every raster stage, calling fragment pixel, and compute; a ray tracing stage is that kind of error as well.
///
/// Every target's text carries its final addresses, HLSL's registers included, so slib's binding pass never runs behind this edge.
/// It needs no toolchain of its own, so it exists wherever `inner` does.
[[nodiscard]] std::unique_ptr<shader_compiler> create_sgl_compiler(std::unique_ptr<shader_compiler> inner);
} // namespace slib
