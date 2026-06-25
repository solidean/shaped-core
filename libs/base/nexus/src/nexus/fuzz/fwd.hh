#pragma once

#include <clean-core/fwd.hh>

// Forward declarations and index vocabulary for the nx::fuzz API-sequence fuzzer.
//
// The engine models a fuzz program in SSA-like form: every value lives in a "slot" addressed by
// (type, value) integer indices, and every step is an operation reading some slots and writing one.
// Plain integer indices (not strong enums) keep the hot generation/minimization loops allocation-light
// and cheap to copy; -1 is the universal "invalid / none" sentinel.

namespace nx::fuzz
{
using namespace cc::primitive_defines;

/// Index into the machine's interned type table. -1 means invalid (also used for void returns).
using type_index = int;
/// Index of an operation within the machine.
using op_index = int;
/// Index of a value within its type's slot vector. A value equal to the current slot count means
/// "append a new slot" (this is how the reachable state grows during generation).
using value_index = int;

inline constexpr int invalid_index = -1;

/// Addresses one value in the state: which type, and which slot of that type.
struct typed_value_index
{
    type_index type = invalid_index;
    value_index value = invalid_index;

    [[nodiscard]] bool is_valid() const { return type != invalid_index && value != invalid_index; }
};

struct fuzz_value;
struct fuzz_operation;
struct executed_operation;
struct fuzz_machine;
struct fuzz_runner;
struct fuzz_run;
struct test;
} // namespace nx::fuzz
