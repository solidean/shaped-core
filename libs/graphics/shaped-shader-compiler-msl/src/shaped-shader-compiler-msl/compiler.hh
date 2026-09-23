#pragma once

#include <clean-core/error/result.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-shader-compiler-msl/compile_options.hh>
#include <shaped-shader-compiler-msl/fwd.hh>
#include <shaped-shader-compiler-msl/shader_description.hh>

/// A lean wrapper over Apple's Metal shader toolchain.
/// Compiles MSL into an sg::compiled_shader: a blob plus the reflected bindings sg builds pipelines from.
///
/// Two artifacts, because Metal has two and only one of them needs a toolchain.
/// A metallib is what `xcrun metal` writes, and MSL source is what the driver compiles when a pipeline is built —
/// `compile_options::artifact` picks, and the `sg::shader_format` on the result says which was produced.
///
/// Reflection is read out of the MSL text either way.
/// A metallib carries no reflection container a tool can read without a GPU, so the text is the only source there is.

/// What the host's Metal toolchain is, or why there is none.
/// A metallib arm needs one; a source arm never does.
struct ssc::msl::toolchain_info
{
    /// True when `xcrun -f metal` resolved to an executable.
    bool is_available = false;

    /// The compiler's own version, e.g. "32023.883"; empty when there is none.
    /// Belongs in any cache key that outlives the process, since Apple ships the toolchain as an updatable component.
    cc::string version;

    /// Absolute path to the resolved `metal` driver; empty when there is none.
    cc::string driver_path;
};

class ssc::msl::compiler
{
public:
    /// Resolves the Metal toolchain once and reports what it found.
    /// Never fails for want of a toolchain: a host without one still compiles, emitting MSL source.
    [[nodiscard]] static cc::result<compiler> create();

    compiler(compiler const&) = delete;
    compiler& operator=(compiler const&) = delete;
    compiler(compiler&&) noexcept;
    compiler& operator=(compiler&&) noexcept;
    ~compiler();

    /// Compiles one entry point of `desc.source`.
    ///
    /// The error carries the Metal compiler's own diagnostics where it ran, and this wrapper's where it did not —
    /// an entry point the text does not declare, a stage Metal has no shape for, or a binding declaration sg has no
    /// `binding_type` for.
    [[nodiscard]] cc::result<sg::compiled_shader> compile(shader_description const& desc,
                                                          compile_options const& options = {});

    /// What `create()` found.
    /// A caller deciding whether to ask for a metallib reads this first.
    [[nodiscard]] toolchain_info const& toolchain() const;

private:
    struct state;
    explicit compiler(cc::unique_ptr<state> s);
    cc::unique_ptr<state> _state;
};
