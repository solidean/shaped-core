#pragma once

#include <clean-core/container/vector.hh>
#include <nexus/fuzz/fwd.hh>

namespace nx::fuzz
{
/// One recorded step of a fuzz program: which operation ran, where its inputs came from, and where
/// its result went. A program (fuzz_run) is just a vector of these, which makes runs cheap to copy,
/// analyze, and minimize.
struct executed_operation
{
    /// Per-step generator state, rolled for every operation. If the operation consumes a cc::random&,
    /// that generator is reconstructed via cc::random::from_state(state), which makes runs replayable.
    cc::u64 state = 0;

    op_index operation = op_index::invalid;

    /// Where each argument is read from. For a cc::random& argument the slot's type is the machine's
    /// random type and the value index is unused (the generator is synthesized from `state`).
    cc::vector<typed_value_index> arg_slots;

    /// Where the result is written. type == invalid for void operations. A value index equal to the
    /// current slot count of the return type means "append a new slot".
    typed_value_index return_slot;

    /// True for invariant checks whose bool result must be true (false bool == invariant violated).
    bool result_must_be_true = false;
};
} // namespace nx::fuzz
