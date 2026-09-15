#include "metal_binding_layout.hh"

namespace sg::backend::metal
{
isize metal_binding_group_layout::argument_slot_count() const
{
    // An array binding occupies `count` consecutive slots from its index, so the highest occupied slot is not simply
    // the highest index.
    isize slots = 0;
    for (auto const& b : bindings())
        slots = cc::max(slots, isize(b.index) + cc::max(isize(1), isize(b.count)));
    return slots;
}
} // namespace sg::backend::metal
