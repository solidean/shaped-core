#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-shader-library/fwd.hh>

/// The language a shader is authored in.
/// A package declares one; the target format comes from the context you acquire with, so the same source can feed several backends.
enum class slib::shader_language
{
    hlsl,
    wgsl, ///< WebGPU's own language: handed on as source, reflected by slib (see compiler/wgsl_compiler.hh)
    sgl, ///< our own: written as the text of the target's language, then compiled as that (see compiler/sgl_compiler.hh)
};

namespace slib
{

/// Resolves an `#include` to its source text, or nullopt if there is no such file.
/// slib backs this with the package's filesystem and records every path it resolves, which is what makes a shader reload when a file it includes changes.
using include_resolver = cc::function_ref<cc::optional<cc::string>(cc::string_view path)>;

} // namespace slib

/// A binding the compiler will reflect under one name, and the name the host knows it by.
struct slib::binding_rename
{
    cc::string reflected;
    cc::string name;
};

/// What `preprocess` hands back.
/// `entry_point` is empty where preprocessing kept the name it was given, which is every compiler but SGL's:
/// SGL renames an entry point the target reserves, and the compile has to ask for the name the text declares.
/// `renamed_bindings` is empty likewise: SGL's text declares `work_values` for what the host binds as `work.values`,
/// and the library renames the compiled shader's reflected bindings with it once the compile settles.
struct slib::preprocessed_source
{
    cc::string source;
    cc::string entry_point;
    cc::vector<binding_rename> renamed_bindings;
    /// A pixel entry point's render targets: how many, and the name of the struct that declares them; -1 and empty
    /// otherwise, and for every compiler but SGL's.
    i32 color_targets = -1;
    cc::string target_struct;
    /// What a device needs to run the shader, which the library sets on the compiled shader.
    /// nullopt is unknown, which every compiler but SGL's is: nothing in HLSL or WGSL declares it.
    cc::optional<sg::feature_set> required_features;
};

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

    /// Flattens `#include`s through `resolve`. The error carries the compiler's own diagnostics.
    ///
    /// Per target, not once for all of them: a compiler targeting SPIR-V flattens with its own macros defined, so a
    /// source may fork on the target it is being built for.
    /// That is why `shader_asset` keeps a flattened source and its dependencies per format entry.
    [[nodiscard]] virtual cc::result<preprocessed_source> preprocess(shader_source_description const& desc,
                                                                     include_resolver resolve) const = 0;

    /// Already-flattened source -> bytecode.
    /// A compile failure arrives as an error on the returned node rather than a throw: a broken shader edit must not take down a running app.
    [[nodiscard]] virtual sg::async_compiled_shader compile(shader_source_description const& desc) const = 0;
};
