#pragma once

#include <shaped-shader-library/compiler/shader_compiler.hh>

#include <memory>

namespace slib
{
/// The compiler for a package authored in SGL: one edge `sgl -> inner->target_format()` over the compiler that builds that format.
///
/// `preprocess` runs SGL's whole pipeline and hands back the text of the target `inner` compiles:
/// HLSL for dx12 over a dxil compiler, HLSL for vulkan over a spirv one, WGSL over the wgsl one, MSL over a metal_lib one.
/// slib has no metal_lib compiler yet, so that last edge is there for whoever brings one, and its text has met no Metal compiler.
/// So the flattened source a `shader_asset` keeps and a compiler's cache hashes IS the emitted text, and `compile` is `inner`'s.
/// Reflection is `inner`'s too: whatever it reads out of that text is what the shader reports.
///
/// `inner` must not be null, and its format must be dxil, spirv, wgsl or metal_lib.
/// Its `preprocess` is never called: SGL has no `#include`, and the emitted text has none either.
///
/// An SGL error is a `preprocess` error, so it rides the failure channel a DXC error does, one line per diagnostic:
/// `cube_shaders/cube.sgl:12:5: error: unknown-name: foo`.
/// The stages SGL has are vertex, fragment, which it calls pixel, and compute; any other stage is that kind of error as well.
///
/// The emitted HLSL names each resource's group with `#pragma sc group N` and no register, so slib's binding pass runs
/// behind this edge as it does behind HLSL; the WGSL and MSL text carries its final addresses.
/// It needs no toolchain of its own, so it exists wherever `inner` does.
[[nodiscard]] std::unique_ptr<shader_compiler> create_sgl_compiler(std::unique_ptr<shader_compiler> inner);
} // namespace slib
