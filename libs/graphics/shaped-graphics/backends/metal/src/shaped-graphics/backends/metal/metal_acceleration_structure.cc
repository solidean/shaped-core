#include "metal_acceleration_structure.hh"

#include <shaped-graphics/backends/metal/metal_context.hh>

namespace sg::backend::metal
{
void metal_accel_storage::release(cc::vector<cc::unique_function<void()>>& finalizers) const
{
    // A second call owns nothing — but may still carry finalizers, which are the caller's feedback point and must run
    // exactly once.
    if (_accel == nullptr && finalizers.empty())
        return;

    auto* const accel = _accel;
    _accel = nullptr;

    // Out of the residency set first, so the set stops naming an allocation that is on its way out.
    if (accel != nullptr)
        _ctx.residency().remove(accel);

    // Handed to the epoch rather than released here: a submitted command buffer may still be tracing against this, and
    // the epoch is what knows when it is not.
    _ctx.epochs().defer(
        [accel, taken = cc::move(finalizers)]() mutable
        {
            if (accel != nullptr)
                accel->release();
            for (auto& f : taken)
                f();
        });
}
} // namespace sg::backend::metal
