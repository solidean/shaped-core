#pragma once

#include <clean-core/error/optional.hh>
#include <shaped-shader-library/compiler/shader_compiler.hh>
#include <shaped-shader-library/fwd.hh>

#include <memory>

namespace bcache
{
class blob_cache;
}

/// What an SGL shader does where an operation of it has no value: an integer division by zero, an array index outside
/// the array (SGL's LEGAL-65).
enum class slib::sgl_undefined_guard : slib::u8
{
    /// Nothing is guarded, at no cost: such an operation is the target's undefined behaviour.
    none,
    /// The invocation ends before the operation runs, and nothing reports it.
    abort,
    /// The operation and its operands are logged as a failed `assert` is, and the invocation ends as for `abort`.
    /// It needs `diagnostics` to log, and is `abort` without it.
    report,
    /// The operation runs on a substitute operand that has a value, and the invocation goes on, as in WGSL.
    define,
};

/// Where an SGL edge keeps SGL's front end, beside the in-memory tier every SGL edge in the process shares.
struct slib::sgl_compiler_options
{
    /// The store a preprocess is kept in across runs.
    /// Unset is `bcache::default_cache()`, looked up at each compile and left out under `SC_SG_COLD=shaders`.
    /// A null store keeps it in memory alone, which is what a test that must not touch the user's cache passes.
    /// A store given must outlive the compiler and answer on its own thread, since a lookup only waits for it.
    /// There is no persistent tier on wasm or without threads, whatever is given.
    cc::optional<bcache::blob_cache*> persistent_cache = {};
    /// Writes every print and failed assert into sg's debug window, and states each site on the shader (EMIT-183).
    /// Off, the text holds neither.
    bool diagnostics = false;
    sgl_undefined_guard guards_undefined = sgl_undefined_guard::none;
    /// Keeps each SGL module's checked layer in `persistent_cache` too, so a later process loads its modules rather
    /// than checking them; off by default, since a process that checks its modules once pays little for it.
    /// The library fetches them ahead of its compiles (`shader_library::prefetch_modules`) and a compile never waits:
    /// one that comes first checks its modules.
    bool keeps_module_artifacts = false;
};

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
/// `preprocess` is cached under everything it reads, in memory and in the store `options` names.
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
[[nodiscard]] std::unique_ptr<shader_compiler> create_sgl_compiler(std::unique_ptr<shader_compiler> inner,
                                                                   sgl_compiler_options const& options = {});

/// The edge `sgl -> spirv` with no compiler behind it: SGL writes the module itself, and an entry point it has no
/// lowering for yet fails to compile.
[[nodiscard]] std::unique_ptr<shader_compiler> create_sgl_spirv_compiler(sgl_compiler_options const& options = {});
} // namespace slib
