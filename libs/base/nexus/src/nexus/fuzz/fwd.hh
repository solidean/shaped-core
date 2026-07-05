#pragma once

#include <clean-core/fwd.hh>

namespace nx
{
struct typed_value; // the fuzz engine's value slots (see nexus/tests/typed_value.hh)
}

// Forward declarations and index vocabulary for the nx::fuzz API-sequence fuzzer.
//
// The engine models a fuzz program in SSA-like form: every value lives in a "slot" addressed by
// (type, value) indices, and every step is an operation reading some slots and writing one. The three
// index roles are distinct strong enums so the compiler rejects mixing them up; each carries its own
// `invalid` sentinel (used for "none", and for void returns on type_index). Crossing into the
// underlying int for subscripting or arithmetic is an explicit `int(x)` at the use site.

namespace nx::fuzz
{
using namespace cc::primitive_defines;

/// Index into the machine's interned type table. `invalid` also denotes a void return.
enum class type_index : int
{
    invalid = -1
};
/// Index of an operation within the machine.
enum class op_index : int
{
    invalid = -1
};
/// Index of a value within its type's slot vector. A value equal to the current slot count means
/// "append a new slot" (this is how the reachable state grows during generation).
enum class value_index : int
{
    invalid = -1
};

/// Addresses one value in the state: which type, and which slot of that type.
struct typed_value_index
{
    type_index type = type_index::invalid;
    value_index value = value_index::invalid;

    [[nodiscard]] bool is_valid() const { return type != type_index::invalid && value != value_index::invalid; }
};

struct fuzz_operation;
struct executed_operation;
struct fuzz_machine;
struct fuzz_runner;
struct fuzz_run;
struct test;
} // namespace nx::fuzz
