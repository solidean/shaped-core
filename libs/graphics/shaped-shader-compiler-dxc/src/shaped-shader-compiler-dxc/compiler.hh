#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/compiled_shader.hh>
#include <shaped-shader-compiler-dxc/compile_options.hh>
#include <shaped-shader-compiler-dxc/preprocessed_source.hh>
#include <shaped-shader-compiler-dxc/shader_description.hh>

#include <memory>

/// A lean wrapper over the DirectX Shader Compiler (DXC, IDxcCompiler3). Compiles HLSL into an
/// sg::compiled_shader (bytecode + reflected bindings + compute workgroup size). Two steps:
///   1. preprocess() — expand macros + resolve #includes via a caller-supplied resolver
///   2. compile()    — turn already-preprocessed source into bytecode (rejects stray #includes)
/// The DXC headers are kept out of this header (pimpl). See the library readme / cheat-sheet.

namespace ssc::dxc
{
/// Resolves an `#include` path to its source text during preprocess(), or nullopt if not found
/// (DXC then reports the include as an error). Non-owning — the callable must outlive the call.
using include_resolver = cc::function_ref<cc::optional<cc::string>(cc::string_view path)>;

class compiler
{
public:
    /// Creates a compiler (one IDxcUtils + IDxcCompiler3). Fails only on a broken DXC install.
    /// A compiler instance is not thread-safe; use one per thread.
    [[nodiscard]] static cc::result<compiler> create();

    compiler(compiler const&) = delete;
    compiler& operator=(compiler const&) = delete;
    compiler(compiler&&) noexcept;
    compiler& operator=(compiler&&) noexcept;
    ~compiler();

    /// Expands macros and inlines `#include`s, without generating bytecode. `resolve_include` maps
    /// an include path to source text. Returns the flattened source; error carries DXC diagnostics.
    [[nodiscard]] cc::result<preprocessed_source> preprocess(shader_description const& desc,
                                                             include_resolver resolve_include,
                                                             compile_options const& options = {});

    /// Compiles fully-preprocessed source into an sg::compiled_shader. `desc.source` must not
    /// contain `#include`s (they are rejected). Error carries DXC diagnostics; a resource kind sg
    /// has no binding_type for yet (texture/sampler/...) is also reported as an error.
    [[nodiscard]] cc::result<sg::compiled_shader> compile(shader_description const& desc,
                                                          compile_options const& options = {});

private:
    struct state;
    explicit compiler(std::unique_ptr<state> s);
    std::unique_ptr<state> _state;
};
} // namespace ssc::dxc
