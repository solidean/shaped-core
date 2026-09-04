#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/string/print.hh>
#include <shaped-graphics/barrier/command_list_slot.hh>

namespace sg
{
command_list_slot command_list_slot_allocator::acquire()
{
    return _state.lock(
        [](state& s) -> command_list_slot
        {
            ++s.live;

            // A SNAPSHOT rather than an accumulate: this is how many exist right now, and summing that would be
            // meaningless.
            // sg counted these already, purely to size a bitmask; nothing ever reported the number.
            CC_RECORD_STAT("sg.command_lists.live", cc::rec::unit_count, s.live);

            // Fast path: a free bit exists in the base 64. Lowest clear bit = trailing ones of the mask.
            if (~s.bits != 0)
            {
                int const i = cc::count_trailing_ones(s.bits);
                s.bits |= (u64(1) << i);
                return command_list_slot(i);
            }

            // Overflow: all 64 base slots are live.
            // Almost always a leaked command list, never submitted or dropped — warn once, then serve from a heap free-list.
            if (!s.overflow_warned)
            {
                s.overflow_warned = true;
                CC_LOG_WARNING("more than 64 concurrent command lists — likely a command list that was never "
                               "submitted or dropped");
            }
            for (isize j = 0; j < s.overflow.size(); ++j)
                if (!s.overflow[j])
                {
                    s.overflow[j] = true;
                    return command_list_slot(64 + int(j));
                }
            s.overflow.push_back(true);
            return command_list_slot(64 + int(s.overflow.size()) - 1);
        });
}

void command_list_slot_allocator::release(command_list_slot slot)
{
    int const i = int(slot);
    CC_ASSERT(i >= 0, "releasing an invalid command_list_slot");
    _state.lock(
        [i](state& s)
        {
            CC_ASSERT(s.live > 0, "releasing a command_list_slot when none are live");
            --s.live;
            CC_RECORD_STAT("sg.command_lists.live", cc::rec::unit_count, s.live);
            if (i < 64)
            {
                u64 const mask = u64(1) << i;
                CC_ASSERT((s.bits & mask) != 0, "releasing a command_list_slot that is not live");
                s.bits &= ~mask;
            }
            else
            {
                isize const j = i - 64;
                CC_ASSERT(j < s.overflow.size() && s.overflow[j], "releasing an overflow slot that is not live");
                s.overflow[j] = false;
            }
        });
}

int command_list_slot_allocator::live_count()
{
    return _state.lock([](state& s) { return s.live; });
}
} // namespace sg
