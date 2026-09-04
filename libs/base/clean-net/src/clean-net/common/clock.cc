#include "clock.hh"

#include <clean-core/common/asserts.hh>
#include <clean-core/common/time.hh>
#include <clean-core/platform/leak_annotations.hh>

namespace cnet
{
namespace
{
class system_clock_impl final : public clock
{
public:
    [[nodiscard]] i64 now_ns() override
    {
        // The steady clock's origin is arbitrary, which is exactly the contract here: only differences are meaningful.
        return i64(cc::current_time_steady_secs() * 1e9);
    }
};
} // namespace

clock& system_clock()
{
    // Deliberately never destroyed: a connection torn down during static destruction still reads a deadline, and a
    // destroyed clock is a worse outcome than a leaked one.
    static auto* const instance = []
    {
        auto const leak = cc::leak_scope();
        return new system_clock_impl();
    }();
    return *instance;
}

void manual_clock::set_ns(i64 ns)
{
    CC_ASSERT(ns >= _now_ns.load(), "a monotonic clock must not move backwards");
    _now_ns.store(ns);
}
} // namespace cnet
