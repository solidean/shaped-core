#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/compiled_shader.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-shader-library/fwd.hh>

namespace slib
{
/// The language a shader is authored in. A package declares one; the target format comes from the
/// context you acquire with, so the same source can feed several backends.
enum class shader_language
{
    hlsl,
    // Planned: slang, glsl, wgsl.
};

/// Resolves an `#include` to its source text, or nullopt if there is no such file. slib backs this with
/// the package's filesystem and records every path it resolves, which is what makes a shader reload when
/// a file it includes changes.
using include_resolver = cc::function_ref<cc::optional<cc::string>(cc::string_view path)>;

/// One shader to compile. `source` is the shader text — flattened once preprocess has run.
struct shader_source_description
{
    cc::string source;
    cc::string entry_point;
    sg::shader_stage stage = sg::shader_stage::compute;
};

/// One compilation edge: `source_language` -> `target_format`. Register implementations on a
/// shader_library, which picks the edge connecting a package's language to a format the target context
/// accepts.
///
/// Implementations must be safe to call from several threads at once — a reload compiles on the watcher
/// thread while a consumer may compile on its own. (ssc::dxc's compiler is one-per-thread, so its
/// adapter keeps a thread_local one.)
class shader_compiler
{
public:
    virtual ~shader_compiler() = default;

    [[nodiscard]] virtual shader_language source_language() const = 0;
    [[nodiscard]] virtual sg::shader_format target_format() const = 0;

    /// Flattens `#include`s through `resolve`. The error carries the compiler's own diagnostics.
    [[nodiscard]] virtual cc::result<cc::string> preprocess(shader_source_description const& desc,
                                                            include_resolver resolve) const = 0;

    /// Already-flattened source -> bytecode. A compile failure arrives as an error on the returned node
    /// rather than a throw: a broken shader edit must not take down a running app.
    [[nodiscard]] virtual sg::async_compiled_shader compile(shader_source_description const& desc) const = 0;
};
} // namespace slib
