#include "log.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/platform/stack_capture.hh>
#include <clean-core/record/value.hh>

using namespace cc::primitive_defines;

namespace
{
/// How deep a stacktrace-enriched event captures.
constexpr isize max_captured_frames = 64;

/// The addresses a stacktrace event carries, plus the cycle reading taken after the capture.
///
/// Capture is orders of magnitude more expensive than the event it decorates, so the end time is recorded too.
/// That makes the cost measured rather than modelled, exactly as the cold path's own accounting is.
constexpr cc::rec::field stacktrace_fields[] = {
    {.name = "capture_end_cycles", .type = cc::rec::type_code::u64_, .offset = 0, .size = 8},
    {.name = "frame_count", .type = cc::rec::type_code::u32_, .offset = 8, .size = 4},
};

constexpr cc::rec::desc stacktrace_desc = {
    .kind = cc::rec::event_kind::value,
    .enable_bit = cc::rec::enable_bit_of(cc::rec::category::logging),
    .name = "record.stacktrace",
    .dom = &cc::rec::g_system_domain,
    .fields = stacktrace_fields,
    .field_count = 2,
    .fixed_payload_size = cc::rec::desc::variable_payload,
};
} // namespace

void cc::rec::impl::log_write(cc::rec::desc const& d, cc::format_string<> fmt)
{
    // The text is the descriptor's name, so the event is a header and nothing else.
    (void)fmt;
    rec::record_event(d);

    if (log_needs_policy(d)) [[unlikely]]
        log_apply_policy(d);
}

void cc::rec::impl::log_apply_policy(cc::rec::desc const& d)
{
    if (d.dom->captures_stacktrace(d.lvl))
    {
        void* frames[max_captured_frames] = {};
        auto const count = cc::capture_stack(cc::span<void*>(frames, max_captured_frames), 2);

        auto writer = rec::open_event(stacktrace_desc, 12 + count * isize(sizeof(void*)));
        if (writer.is_open())
        {
            auto const out = writer.payload();
            auto const end_cycles = cc::current_cycles();
            auto const frame_count = u32(count);

            if (out.size() >= 12)
            {
                cc::memcpy(out.data(), &end_cycles, sizeof(end_cycles));
                cc::memcpy(out.data() + 8, &frame_count, sizeof(frame_count));
                auto const room = cc::min(count, (out.size() - 12) / isize(sizeof(void*)));
                if (room > 0)
                    cc::memcpy(out.data() + 12, frames, size_t(room) * sizeof(void*));
                writer.commit(12 + room * isize(sizeof(void*)),
                              rec::impl::flag_has_stacktrace | rec::impl::flag_has_end_cycles);
            }
        }
    }

    if (d.dom->breaks_on(d.lvl))
        CC_IMPL_DEBUG_BREAK();
}
