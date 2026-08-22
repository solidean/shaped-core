#include "pinned_value.hh"

#include <clean-core/record/impl/writer_tls.hh>

#include <memory> // std::shared_ptr, which is what a pinned_data's owner is

using namespace cc::primitive_defines;

namespace
{
/// A heap copy of a pinned_data's shared owner, which is what the chunk actually holds.
///
/// A `cc::pin` is a `void*` plus a release function, and a shared_ptr is two words — so it cannot be stored in the
/// slot directly and one small allocation per pinned event is the price.
/// That is the right trade for a debugging affordance: the alternative is copying the bytes themselves, which is the
/// cost this whole path exists to avoid.
using pin_owner = std::shared_ptr<void const>;

void release_pin_owner(void* object)
{
    delete static_cast<pin_owner*>(object);
}
} // namespace

bool cc::rec::impl::record_pinned_bytes(cc::rec::desc const& d, cc::pinned_data<byte const> const& bytes)
{
    // Reserved BEFORE the pin is taken, so a rotation cannot land between the two and leave the reference on a chunk
    // the event does not end up in.
    auto writer = rec::open_event(d, isize(sizeof(pinned_payload)));
    if (!writer.is_open())
        return false;

    auto const out = writer.payload();
    if (out.size() < isize(sizeof(pinned_payload)))
        return false;

    auto* const chunk = t_writer.current;
    if (chunk == nullptr)
        return false;

    // An empty view has nothing to keep alive, so it needs no pin — and taking one on a null owner would spend an
    // allocation to hold nothing.
    if (!bytes.empty())
    {
        auto owner = bytes.pin();
        if (owner == nullptr)
            return false; // a borrowed view with no owner cannot be pinned, and recording its address would lie

        auto* const held = new (std::nothrow) pin_owner(cc::move(owner));
        if (held == nullptr)
            return false;

        if (!chunk->try_add_pin({.object = held, .release = &release_pin_owner}))
        {
            delete held;
            return false;
        }
    }

    auto const payload
        = pinned_payload{.data = u64(reinterpret_cast<uintptr_t>(bytes.data())), .size = u64(bytes.size())};
    cc::memcpy(out.data(), &payload, sizeof(payload));

    writer.commit(isize(sizeof(payload)), rec::impl::flag_payload_pinned);
    return true;
}
