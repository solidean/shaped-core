#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-shader-compiler-msl/fwd.hh>

/// What a compile produces, and the flags that shape it.

/// Which artifact a compile is asked for.
///
/// Metal has two, and they are not the same kind of thing.
/// A metallib is AIR in a portable container, which is what `sg::shader_format::metal_lib` means and what survives the
/// machine that produced it; producing one needs Apple's Metal toolchain, a component installed separately from Xcode.
/// MSL source is the text itself, compiled by the driver when a pipeline is built — no toolchain, nothing to ship.
enum class ssc::msl::artifact_kind
{
    /// A metallib where the toolchain is there, MSL source where it is not.
    /// The `sg::shader_format` on the result says which happened, so a caller that does not care never asks.
    automatic,
    /// A metallib, or an error naming the missing toolchain.
    /// What a build that ships bytecode asks for, since a source blob would fail at the machine it shipped to.
    metallib,
    /// MSL source, whatever the host has.
    msl_source,
};

/// The optimization the Metal compiler is asked for; ignored by a source-only compile, which compiles nothing.
enum class ssc::msl::optimization_level
{
    disabled, ///< -O0
    level_1,  ///< -O1
    level_2,  ///< -O2, the default
    level_3,  ///< -O3
};

/// Flags for one compile.
struct ssc::msl::compile_options
{
    artifact_kind artifact = artifact_kind::automatic;
    optimization_level optimization = optimization_level::level_2;

    /// -frecord-sources, so a capture in Xcode shows the shader's own text.
    bool debug_info = false;

    /// -Werror.
    /// The source arm refuses it, since the driver compiles with its default options.
    bool warnings_as_errors = false;

    /// The MSL version, as the `-std=` value spells it — "metal3.2", "metal4.0".
    /// Empty takes whatever the installed toolchain prefers, which moves when Apple ships a new one.
    /// The source arm refuses a non-empty one.
    cc::string language_version;

    /// -D entries, each "NAME" or "NAME=value".
    /// The source arm writes them as `#define` lines in front of the text instead.
    cc::vector<cc::string> defines;

    /// Passed through verbatim, after everything above.
    /// Where a hand-written shader's `-I` goes: this wrapper resolves no includes of its own.
    /// The source arm refuses a non-empty list.
    cc::vector<cc::string> extra_args;
};
