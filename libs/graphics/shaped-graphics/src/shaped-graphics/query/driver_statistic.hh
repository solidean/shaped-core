#pragma once

#include <clean-core/string/string.hh>
#include <shaped-graphics/fwd.hh>

/// One figure a driver reports of a pipeline it compiled: the registers a stage takes, its spills, its instructions.
///
/// The names and their meanings are the driver's, and differ between vendors: Intel says "SIMD width" and "GRF
/// registers", AMD "VGPRs" and "SGPRs".
/// So a statistic compares one pipeline against another on the same driver, not across them.
/// A backend reports them only where its context was asked to capture them, which costs at pipeline creation.
struct sg::driver_statistic
{
    /// The compiled program it is of, as the driver names it: a stage, or a variant of one ("SIMD16 Compute").
    cc::string executable;
    cc::string name;
    cc::string description;
    /// The value, a boolean as 0 or 1.
    f64 value = 0;
};

/// One form a driver keeps of a pipeline's code: its own IR at some stage, or the machine code, as text.
///
/// What forms there are is the driver's: AMD's last is the ISA's disassembly, Intel's its own assembly.
/// A backend hands them back only where its context was asked to capture them, which costs more than the statistics.
struct sg::driver_representation
{
    /// The compiled program it is of, as `driver_statistic::executable` names it.
    cc::string executable;
    cc::string name;
    cc::string description;
    /// The form as text; empty where the driver gave it as binary data, which is not kept.
    cc::string text;
};
