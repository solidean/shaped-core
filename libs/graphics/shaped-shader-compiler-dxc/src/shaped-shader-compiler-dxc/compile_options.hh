#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>

/// Compilation knobs, mapped to DXC command-line flags. Closed enums (not raw strings/ints) so the
/// valid set is fixed and typo-proof; each maps to one flag internally (see command_line_args.cc).

namespace ssc::dxc
{
/// Bytecode format DXC should emit. DXIL (for dx12) is the only target wired today; spirv/metal_lib
/// slot in later behind the same option.
enum class compile_target
{
    dxil,
};

/// HLSL shader model — becomes the profile suffix (`sm_6_8` -> "6_8", e.g. profile "cs_6_8").
enum class shader_model
{
    sm_6_0,
    sm_6_1,
    sm_6_2,
    sm_6_3,
    sm_6_4,
    sm_6_5,
    sm_6_6,
    sm_6_7,
    sm_6_8,
};

/// Optimization level — one DXC flag. `disabled` is `-Od` (optimizations off, source semantics
/// preserved, best for stepping); `level_0`..`level_3` are `-O0`..`-O3`.
enum class optimization_level
{
    disabled,
    level_0,
    level_1,
    level_2,
    level_3,
};

/// Options for a single compile. Defaults produce optimized, warnings-as-errors DXIL.
struct compile_options
{
    compile_target target = compile_target::dxil;
    optimization_level optimization = optimization_level::level_3;

    /// `-Zi -Qembed_debug`: embed source/variable/line info into the blob for PIX/RenderDoc.
    /// Off by default (it enlarges the bytecode). A side-car PDB (`-Fd`) is not exposed yet.
    bool debug_info = false;

    /// `-WX`: promote every warning to an error.
    bool warnings_as_errors = true;

    /// Preprocessor defines, each `"NAME"` or `"NAME=VALUE"` (passed as `-D`).
    cc::vector<cc::string> defines;

    /// Raw DXC flags appended verbatim — an escape hatch for options this struct does not model.
    cc::vector<cc::string> extra_args;
};
} // namespace ssc::dxc
