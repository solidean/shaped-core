#include "value.hh"

#include <clean-core/common/utility.hh>

void cc::rec::impl::record_text_value(cc::rec::desc const& d, cc::string_view text)
{
    constexpr auto header_bytes = isize(sizeof(u32));

    auto writer = rec::open_event(d, header_bytes + text.size());
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
