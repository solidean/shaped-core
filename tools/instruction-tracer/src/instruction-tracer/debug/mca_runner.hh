#pragma once

#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

namespace itrace
{
/// Raw result of running llvm-mca once: its captured stdout (the -json output) and stderr (the
/// skip warnings). `ran == false` means the process could not be launched at all.
struct mca_run_result
{
    bool ran = false;
    cc::string json;
    cc::string stderr_text;
};

/// Run llvm-mca (`mca_exe`) over `input_asm` (fed on stdin), returning its JSON and stderr.
///
/// `cpu` picks the model: empty means host (`-mcpu=native`) with a graceful fallback to a baseline
/// (`x86-64`, then `skylake`) if native yields nothing; a non-empty value is passed through verbatim.
/// The rest of the command line is fixed (timeline + bottleneck-analysis + skip-unsupported). A soft
/// failure (tool missing, launch error) returns `ran == false`; the caller degrades without timing.
mca_run_result run_llvm_mca(cc::string_view mca_exe, cc::string_view cpu, cc::string_view input_asm);
} // namespace itrace
