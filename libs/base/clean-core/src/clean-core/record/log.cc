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
// The frames were always written; nothing DESCRIBED them, so no consumer could reach them — a crash dump's stacks
// were unreadable through the event API for exactly that reason.
//
// The payload already has the u64_array shape by construction: a count where the field points, and the values right
// after it.
// So this describes bytes that were always there rather than changing any of them.
constexpr cc::rec::field stacktrace_fields[] = {
    {.name = "capture_end_cycles", .type = cc::rec::type_code::u64_, .offset = 0, .size = 8},
    {.name = "frames", .type = cc::rec::type_code::u64_array, .offset = 8, .size = 4},
};

constexpr cc::rec::desc stacktrace_desc = {
    .kind = cc::rec::event_kind::value,
    .enable_bit = cc::rec::enable_bit_of(cc::rec::category::logging),
    .name = "record.stacktrace",
    .dom = &cc::rec::g_system_domain,
    .fields = stacktrace_fields,
    .field_count = u16(CC_ARRAY_COUNT_OF(stacktrace_fields)),
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
        auto const count = cc::capture_stack(cc::span<void*>(frames, max_captured_frames), 2).count;

        // **An address goes in as a u64, whatever a pointer is worth here.**
        //
        // The field says u64_array and a recording is read on a machine other than the one that wrote it, so the
        // payload cannot be pointer-width: on wasm32 a pointer is four bytes, and writing them raw leaves every
        // reader's bounds check short by half, which returns no frames at all rather than wrong ones.
        // sampling.cc writes its own frames this way for the same reason.
        constexpr auto frames_offset = isize(12);
        auto writer = rec::open_event(stacktrace_desc, frames_offset + count * isize(sizeof(u64)));
        if (writer.is_open())
        {
            auto const out = writer.payload();
            auto const end_cycles = cc::current_cycles();

            if (out.size() >= frames_offset)
            {
                // The count describes what was WRITTEN rather than what was captured.
                // A count larger than the payload holds fails the reader's bounds check, so a truncated write
                // claiming the full capture loses every frame instead of the last few.
                auto const room = cc::min(count, (out.size() - frames_offset) / isize(sizeof(u64)));
                auto const frame_count = u32(room);

                cc::memcpy(out.data(), &end_cycles, sizeof(end_cycles));
                cc::memcpy(out.data() + 8, &frame_count, sizeof(frame_count));

                for (auto i = isize(0); i < room; ++i)
                {
                    auto const address = u64(reinterpret_cast<uintptr_t>(frames[i]));
                    cc::memcpy(out.data() + frames_offset + i * isize(sizeof(u64)), &address, sizeof(address));
                }

                writer.commit(frames_offset + room * isize(sizeof(u64)),
                              rec::impl::flag_has_stacktrace | rec::impl::flag_has_end_cycles);
            }
        }
    }

    if (d.dom->breaks_on(d.lvl))
        CC_IMPL_DEBUG_BREAK();
}
