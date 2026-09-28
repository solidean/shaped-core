#include "value.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/record/log.hh>

using namespace cc::primitive_defines;

void cc::rec::impl::record_text_value(cc::rec::desc const& d, cc::string_view text)
{
    constexpr auto header_bytes = isize(sizeof(u32));

    // Whole below the log cap, and cut at the cap past it, so a huge text does not abandon a chunk tail every call.
    auto wanted = header_bytes + text.size();
    if (wanted > rec::impl::log_max_payload) [[unlikely]]
        wanted = rec::impl::log_payload_cap();

    auto writer = rec::open_event(d, wanted, wanted);
    if (!writer.is_open())
        return;

    auto const out = writer.payload();
    if (out.size() < header_bytes)
        return; // not even the length fits; abandoning leaves the chunk untouched

    auto const kept = cc::min(text.size(), out.size() - header_bytes);
    auto const length = u32(kept);
    cc::memcpy(out.data(), &length, sizeof(length));
    if (kept > 0)
        cc::memcpy(out.data() + header_bytes, text.data(), size_t(kept));

    writer.commit(header_bytes + kept, kept < text.size() ? rec::impl::flag_truncated : rec::impl::flag_none);
}

void cc::rec::impl::record_named_value(cc::rec::desc const& d, void const* value, isize value_size, cc::string_view name)
{
    constexpr auto length_bytes = isize(sizeof(u32));

    auto writer = rec::open_event(d, value_size + length_bytes + name.size(), value_size + length_bytes);
    if (!writer.is_open())
        return;

    auto const out = writer.payload();

    // The value is the point of the event and the name only identifies it, so a payload that cannot hold both keeps
    // the value whole and truncates the name.
    // Dropping the event instead would lose the reading, which is the opposite of what a caller asked for.
    if (out.size() < value_size + length_bytes)
        return;

    cc::memcpy(out.data(), value, size_t(value_size));

    auto const kept = cc::min(name.size(), out.size() - value_size - length_bytes);
    auto const length = u32(kept);
    cc::memcpy(out.data() + value_size, &length, sizeof(length));
    if (kept > 0)
        cc::memcpy(out.data() + value_size + length_bytes, name.data(), size_t(kept));

    writer.commit(value_size + length_bytes + kept,
                  kept < name.size() ? rec::impl::flag_truncated : rec::impl::flag_none);
}
