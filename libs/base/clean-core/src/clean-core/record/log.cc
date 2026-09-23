#include "log.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/platform/stack_capture.hh>
#include <clean-core/record/value.hh>
#include <clean-core/record/writer.hh>

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

/// Whether this thread's format buffer has been built yet, and whether it is still there.
///
/// Three states rather than a bool, because "not yet" and "gone" both read as absent and only one of them may
/// construct the buffer.
/// The variable is constant-initialized and trivially destructible, so it stays readable for the whole thread —
/// including after every thread_local with a destructor has run.
enum class scratch_state : u8
{
    unborn = 0,
    alive,
    dead,
};
thread_local scratch_state tl_scratch_state = scratch_state::unborn;

/// Owns the buffer and publishes its lifetime through `tl_scratch_state`.
struct scratch_holder
{
    cc::string buffer = cc::string::create_with_capacity(cc::rec::impl::log_scratch_capacity);

    scratch_holder() { tl_scratch_state = scratch_state::alive; }
    ~scratch_holder() { tl_scratch_state = scratch_state::dead; }

    scratch_holder(scratch_holder const&) = delete;
    scratch_holder& operator=(scratch_holder const&) = delete;
};
} // namespace

cc::string* cc::rec::impl::log_scratch()
{
    // Checked before the buffer is touched: reaching a destroyed thread_local is what this exists to prevent, and a
    // late record on a dying thread is a case the writer's own exit handshake already plans for.
    if (tl_scratch_state == scratch_state::dead) [[unlikely]]
        return nullptr;

    // Built on first use, which is what sets the state to alive.
    thread_local scratch_holder holder;
    return &holder.buffer;
}

isize cc::rec::impl::log_payload_cap()
{
    auto const room = impl::max_event_payload();
    return room > 0 ? cc::min(log_max_payload, room) : log_max_payload;
}

void cc::rec::impl::log_emit(cc::rec::desc const& d, cc::string_view text)
{
    // The reservation is what the message actually needs, so a short chunk tail is left behind rather than deciding
    // where the message ends.
    //
    // The cap is only consulted for a message that could possibly exceed it, since reaching it asks the pool for the
    // configured chunk size and an ordinary message is three orders of magnitude below it.
    //
    // At least one byte: an empty formatted message still gets an event, and open_event's contract has no zero.
    auto wanted = text.size() > 0 ? text.size() : isize(1);
    if (wanted > log_max_payload) [[unlikely]]
        wanted = log_payload_cap();

    auto writer = rec::open_event(d, wanted, wanted);
    if (!writer.is_open())
        return; // the pool had nothing to give; open_event counted the loss for the next gap event

    auto const out = writer.payload();
    auto const kept = cc::min(text.size(), out.size());
    if (kept > 0)
        cc::memcpy(out.data(), text.data(), size_t(kept));

    // Committing the FULL size rather than what was copied: commit clamps to the reservation and flags the cut, so
    // the cap and a message larger than one whole chunk are reported through the same path.
    writer.commit(text.size());
}

void cc::rec::impl::log_shrink_scratch(cc::string& scratch)
{
    scratch = cc::string::create_with_capacity(log_scratch_capacity);
}

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
