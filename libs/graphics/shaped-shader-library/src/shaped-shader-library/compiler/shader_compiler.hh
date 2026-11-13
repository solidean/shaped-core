#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-shader-library/compiler/block_layout.hh>
#include <shaped-shader-library/fwd.hh>

/// The language a shader is authored in.
/// A package declares one; the target format comes from the context you acquire with, so the same source can feed several backends.
enum class slib::shader_language
{
    hlsl,
    wgsl, ///< WebGPU's own language: handed on as source, reflected by slib (see compiler/wgsl_compiler.hh)
    sgl, ///< our own: written as the text of the target's language, then compiled as that (see compiler/sgl_compiler.hh)
    metal, ///< MSL, for a shader hand-written against metal (see compiler/metal_compiler.hh)
};

namespace slib
{

/// Resolves an `#include` to its source text, or nullopt if there is no such file.
/// slib backs this with the package's filesystem and records every path it resolves, which is what makes a shader reload when a file it includes changes.
using include_resolver = cc::function_ref<cc::optional<cc::string>(cc::string_view path)>;

} // namespace slib

/// One file of the module library an SGL source may `use`: its virtual path and its text.
struct slib::module_source
{
    cc::string_view path;
    cc::string_view text;
};

/// The files of every module directory of a library, read: what an SGL compile's `use` is resolved against.
struct slib::module_library
{
    cc::vector<cc::string> paths;
    cc::vector<cc::string> texts;
    /// Each file's revision just before its text was read.
    cc::vector<file_revision> revisions;

    /// Each file as a view of `paths` and `texts`, which must outlive the result.
    [[nodiscard]] cc::vector<module_source> files() const
    {
        auto result = cc::vector<module_source>();
        for (auto i = cc::isize(0); i < paths.size(); ++i)
            result.push_back({.path = paths[i], .text = texts[i]});
        return result;
    }
};

/// What `preprocess` hands back.
/// `entry_point` is empty where preprocessing kept the name it was given, which is every compiler but SGL's:
/// SGL renames an entry point the target reserves, and the compile has to ask for the name the text declares.
struct slib::preprocessed_source
{
    cc::string source;
    cc::string entry_point;
    /// The whole shader but its bytecode and compiler, where the source language states it, which only SGL does.
    /// The library then takes nothing from the compile but those two, and the compiler's reflection only confirms the rest.
    /// `target_set` is the bare name of the target struct, which the library qualifies with the namespace of the file's generated types.
    /// nullopt for every other compiler, whose shader is what the compile reflects.
    cc::optional<sg::compiled_shader> stated;
    /// Every binding the text declares, used or not, which is what a compiler reflecting the text may report.
    /// Empty without `stated`.
    cc::vector<sg::binding> declared_bindings;
    /// Every constant block and buffer element the text declares, laid out as the text states; empty without `stated`.
    cc::vector<block_layout> layouts;
    /// The module files the source reached through `use`, by virtual path: what it depends on beside itself.
    /// Only SGL fills it.
    cc::vector<cc::string> used_modules;
    /// The target's own binary where SGL wrote one, which is then all a compile needs; `source` is the program it was
    /// written from, for a reader.
    cc::vector<byte> bytecode;
    /// What the compiler warned of while writing the text, one diagnostic each, its notes on the lines under it.
    /// Only SGL fills it, and a cached result keeps them; a shader_library logs each once.
    cc::vector<cc::string> warnings;
    /// What the text asks of DXC beyond what its edge always hands it, which the compile of this text appends.
    /// Only SGL fills it: `-Gis` for an entry point whose `@float` keeps the special values (EMIT-185).
    cc::vector<cc::string> dxc_args;
    /// What this entry point holds that the native route does not lower yet, when a context that prefers SGL's binary gets DXC's.
    /// Empty where the binary is SGL's, and where nobody asked for it.
    /// Only SGL fills it, and a shader_library logs each once.
    cc::string native_gap;
};

/// A value one compile gives an option of an SGL source, by the option's name, spelled as SGL spells it: `16`, `true`, `.rgba16_float`.
/// Only SGL has options; every other compiler takes none.
struct slib::shader_option
{
    cc::string name;
    cc::string value;

    [[nodiscard]] bool operator==(shader_option const&) const = default;
};

namespace slib
{
/// The option `shader_asset::acquire` adds for an SGL shader: every `sg::feature` the context has, comma-separated,
/// in the enum's order, which SGL's `has` answers (CHK-426).
/// The SGL compiler takes it as the compile's device, not as an option of the source, and the cache keys on it alike.
inline constexpr cc::string_view k_device_features_option = "device_features";

/// The option `shader_asset::acquire` adds for a context that prefers SGL's own SPIR-V, valued `true`: a compile of an
/// entry point SGL has no lowering for still goes through DXC.
/// `sg::context::prefers_native_spirv`'s; the cache keys on it as on any option.
inline constexpr cc::string_view k_native_spirv_option = "native_spirv";

/// The same for a context that prefers SGL's own DXIL, `sg::context::prefers_native_dxil`'s.
inline constexpr cc::string_view k_native_dxil_option = "native_dxil";

/// The option `shader_asset::acquire` adds, valued `true`, for a context that asks SGL's own SPIR-V and DXIL to carry
/// each operation's `.sgl` line, for a debugger (EMIT-257): `sg::context::writes_native_line_tables`'s.
/// It changes nothing of what DXC compiles, and the cache keys on it as on any option.
inline constexpr cc::string_view k_native_line_table_option = "native_line_table";

/// The option a caller passes to `shader_asset::acquire`, valued `true`, to let a WebGPU pipeline run an SGL entry
/// point at `fast` whose `@float` WGSL cannot keep, with a warning, rather than refusing it (EMIT-185).
/// The host's call, never the shader's, and the cache keys on it as on any option.
inline constexpr cc::string_view k_weakens_float_option = "weakens_float";

/// The option a polyfilled pipeline's acquire passes for an SGL mesh stage, valued `compute` or `vertex`: the half of
/// its polyfill to compile, which is then that stage (SGL's CHK-524).
/// It passes `vertex` for the pipeline's pixel stage too, which then takes what the mesh stage wrote per primitive
/// from the vertex half.
/// The cache keys on it as on any option.
inline constexpr cc::string_view k_mesh_form_option = "mesh_form";

/// The option a pipeline that polyfills its geometry stage passes beside `k_mesh_form_option`: the name of the
/// pipeline's vertex entry point, which the geometry stage's compute half runs on each corner (SGL's CHK-525).
inline constexpr cc::string_view k_polyfill_vertex_option = "polyfill_vertex";

/// The option a pipeline that polyfills its task stage passes beside `k_mesh_form_option` for its mesh stage's compute
/// half and the scan before it, valued `scan` there: the name of the pipeline's task entry point, whose records both
/// read (SGL's CHK-524).
inline constexpr cc::string_view k_polyfill_task_option = "polyfill_task";

/// The option a pipeline that polyfills its tessellation stages passes beside `k_mesh_form_option` for its evaluation
/// stage's compute half: the name of the pipeline's control entry point, whose patches it reads (SGL's CHK-527).
inline constexpr cc::string_view k_polyfill_control_option = "polyfill_control";

/// The option that compiles one of the passes SGL's prelude holds for a polyfill in place of the asset's own entry
/// point, valued its name, such as `sg_tessellation_scan` (SGL's CHK-527); the result is a compute shader.
inline constexpr cc::string_view k_polyfill_pass_option = "polyfill_pass";
} // namespace slib

namespace slib
{
/// An option's value as a compile takes it, from the C++ type a generated options struct holds it as.
[[nodiscard]] shader_option option_of(cc::string_view name, bool value);
[[nodiscard]] shader_option option_of(cc::string_view name, int value);
[[nodiscard]] shader_option option_of(cc::string_view name, u32 value);
/// `value` must be finite; it is spelled so the compile reads back exactly this float.
[[nodiscard]] shader_option option_of(cc::string_view name, float value);
[[nodiscard]] shader_option option_of(cc::string_view name, sg::pixel_format value);
} // namespace slib

/// One shader to compile.
/// `source` is the shader text — flattened once preprocess has run.
struct slib::shader_source_description
{
    cc::string source;
    cc::string entry_point;
    sg::shader_stage stage = sg::shader_stage::compute;
    /// What a diagnostic calls the source: a virtual path, or the label of an ad-hoc compile; may be empty.
    /// Never opened, and no part of what a compile depends on.
    cc::string label;
    /// The values SGL's `preprocess` writes the text with; what it writes is all a compile reads, so the cache keys on that.
    cc::vector<shader_option> options;
    /// What the text needs of a device, where the source language states it, which only SGL does; set by the library
    /// between preprocess and compile.
    /// A compiler picks what its target needs from it: DXC compiles at Shader Model 6.9 for shader execution reordering.
    sg::feature_set required_features;
    /// The module library an SGL source may `use`, which every other language ignores.
    /// A file of it whose path is `label` is the source itself, and is left out.
    cc::span<module_source const> modules;
    /// Raw DXC flags this compile needs, which a DXC compiler appends and its cache keys on; every other compiler ignores them.
    /// SGL's edge sets what its text needs, so a caller wiring a DXC compiler behind it passes nothing.
    cc::vector<cc::string> dxc_args;
    /// The oldest MSL version the text compiles at, as `-std=` spells it: "metal3.2"; empty takes the toolchain's default.
    /// Only a metal compiler building a metallib reads it, since the source arm's driver compiles at the device's newest.
    cc::string metal_language_version;
    /// The package's `SOFTWARE_TRACE`, which only SGL reads: an inline trace takes the software walk on HLSL too.
    bool software_trace = false;
    /// The package's `NATIVE_SPIRV`, which only SGL reads: its SPIR-V is SGL's own, and an entry point with no lowering
    /// yet is an error rather than a compile through DXC.
    bool native_spirv = false;
    /// The package's `NATIVE_DXIL`, the same of its DXIL.
    bool native_dxil = false;
    /// What `preprocess` wrote in place of text, which a compile hands on as it is; empty where it wrote text.
    cc::vector<byte> bytecode;
};

/// One compilation edge: `source_language` -> `target_format`.
/// Register implementations on a shader_library, which picks the edge connecting a package's language to a format the target context accepts.
///
/// Implementations must be safe to call from several threads at once — a reload compiles on the watcher thread while a consumer may compile on its own.
class slib::shader_compiler
{
public:
    virtual ~shader_compiler() = default;

    [[nodiscard]] virtual shader_language source_language() const = 0;
    [[nodiscard]] virtual sg::shader_format target_format() const = 0;

    /// Flattens `#include`s through `resolve`.
    /// The error carries the compiler's own diagnostics.
    ///
    /// Per target, not once for all of them: a compiler targeting SPIR-V flattens with its own macros defined, so a
    /// source may fork on the target it is being built for.
    /// That is why `shader_asset` keeps a flattened source and its dependencies per format entry.
    [[nodiscard]] virtual cc::result<preprocessed_source> preprocess(shader_source_description const& desc,
                                                                     include_resolver resolve) const = 0;

    /// Already-flattened source -> bytecode.
    /// A compile failure arrives as an error on the returned node rather than a throw: a broken shader edit must not take down a running app.
    [[nodiscard]] virtual sg::async_compiled_shader compile(shader_source_description const& desc) const = 0;

    /// Where `shader`'s own bytecode puts the values of each constant block and buffer element, which slib compares
    /// against what SGL stated of the text; nullopt where this compiler cannot read it.
    [[nodiscard]] virtual cc::optional<cc::vector<block_layout>> reflect_layouts(sg::compiled_shader const& shader) const
    {
        (void)shader;
        return {};
    }

    /// Starts fetching what this compiler keeps across runs of the modules `read` reads, where it keeps anything, so
    /// a compile of them that comes after continues from it; it never waits, and neither does a compile.
    /// Ready once every fetch answered, a miss included; at once, and without calling `read`, for a compiler that
    /// keeps nothing.
    [[nodiscard]] virtual cc::shared_async<cc::unit> prefetch_modules(cc::function_ref<module_library()> read) const
    {
        (void)read;
        return cc::make_async_from_value(cc::unit{});
    }
};
