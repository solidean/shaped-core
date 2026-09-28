#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/record/record.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>

#include <type_traits>

// CC_LOG_* — messages, formatted on the calling thread and written straight into the stream.
//
// A message with no arguments costs the stream NOTHING: the text lives in the site's descriptor, so the event is a
// header and no payload at all.
// A message with arguments is formatted once into a per-thread buffer that rests at `log_scratch_capacity`, and the
// finished bytes are then reserved and copied in one piece.
//
// Which domain a message belongs to comes from `cc_rec_domain()`, so a site never names one.
// Levels gate per domain, and a domain can also be told to capture a stack or break into the debugger at a level.

namespace cc::rec::impl
{
/// The ceiling on what one formatted message may claim, whatever the chunks are sized at.
/// It exists so an absurd message cannot abandon an arbitrarily large chunk trying to fit; `log_payload_cap()` is the
/// limit that actually applies.
inline constexpr isize log_max_payload = 1 << 20;

/// The largest message this recorder writes whole right now: `log_max_payload`, or what one chunk holds when the
/// configured chunk is smaller than that.
///
/// **A message below it is never cut by where a chunk happened to end** — the reservation is the message's own size,
/// so a chunk whose tail is too short is left behind for a fresh one.
/// A message past it is truncated and flagged, which is a cut a reader can explain.
/// With the default 1 MiB chunk this is a few hundred bytes under a mebibyte; a build that configures small chunks
/// gets a correspondingly smaller cap.
[[nodiscard]] isize log_payload_cap();

/// What the per-thread format buffer holds between messages.
/// A message that pushes it past this gives the memory back afterwards, so a single huge message does not leave
/// its buffer resident for the life of the thread.
inline constexpr isize log_scratch_capacity = 4096;

/// Writes a message whose text is already in the descriptor, so the event carries no payload.
void log_write(rec::desc const& d, cc::format_string<> fmt);

/// Claims this thread's format buffer, or returns null when it cannot be had; `log_write` then formats into a local
/// buffer instead.
///
/// **Null has two causes.**
/// The buffer is already claimed, because a formatter the outer message is running logs a message of its own.
/// Or the buffer is destroyed, because a thread_local destructor logs — which the recorder already contemplates for
/// its write cursor, see the thread exit handshake in `writer.cc`.
///
/// A non-null result must be handed back through `log_release_scratch`, and a null one must not be.
[[nodiscard]] cc::string* log_scratch();

/// Hands back a buffer `log_scratch` claimed, first giving its memory back if a message grew it past
/// `log_scratch_capacity`.
void log_release_scratch(cc::string& scratch);

/// Reserves `text.size()` bytes, taking a fresh chunk when the current one cannot hold them, and publishes the copy.
/// A text past `log_payload_cap()` is truncated and flagged.
void log_emit(rec::desc const& d, cc::string_view text);

/// Runs the domain's per-level stacktrace and debug-break policy.
///
/// **The break lands here rather than at the log site**, one frame down from the code that logged.
/// Putting it at the site would inline the check into every message for the sake of a frame nobody looks at.
CC_COLD_FUNC void log_apply_policy(rec::desc const& d);

/// True when this message has to do more than be written.
[[nodiscard]] CC_FORCE_INLINE bool log_needs_policy(rec::desc const& d)
{
    return d.dom->captures_stacktrace(d.lvl) || d.dom->breaks_on(d.lvl);
}

/// Formats a message and publishes it.
///
/// **The arguments are formatted exactly once**, however the chunks fall.
/// A formatter may have side effects, and a retry that re-ran it would double them — so the message is built whole
/// first and the stream is asked for its finished size, rather than the stream's leftover room deciding where the
/// message ends.
///
/// **Where a chunk happens to end never decides where a message ends.**
/// The only cut is `log_payload_cap()`, which a reader can explain; a cut at an offset that moves with the log volume
/// is not.
///
/// It costs one copy out of the format buffer, and a 4 KiB buffer per logging thread.
///
/// **A formatter may itself log.**
/// The nested message finds the buffer claimed and formats into a local one, so neither text bleeds into the other.
template <class... Args>
    requires(sizeof...(Args) > 0)
void log_write(rec::desc const& d, cc::format_string<std::type_identity_t<Args>...> fmt, Args&&... args)
{
    // Empty and inline, so it costs nothing unless the thread buffer is claimed or gone.
    auto local = cc::string();

    auto* const scratch = impl::log_scratch();
    auto& buffer = scratch != nullptr ? *scratch : local;

    {
        // Only the call that claimed the buffer releases it, or a nested call would free the outer one's claim.
        // A guard rather than a trailing call, so a throwing formatter does not leave the thread on the fallback.
        CC_DEFER
        {
            if (scratch != nullptr)
                impl::log_release_scratch(*scratch);
        };

        buffer.clear();
        cc::format_append(buffer, fmt, cc::forward<Args>(args)...);

        impl::log_emit(d, buffer);
    }

    if (log_needs_policy(d)) [[unlikely]]
        log_apply_policy(d);
}
} // namespace cc::rec::impl

/// Defines this message's site and writes it.
///
/// The format string doubles as the site's name, so every message from one site groups under one string whatever it
/// formatted to — which is what makes "how often does this fire" answerable.
#define CC_REC_IMPL_LOG(level_, fmt_, ...)                                                                            \
    do                                                                                                                \
    {                                                                                                                 \
        CC_REC_DEFINE_DESC(cc_rec_site_desc_, ::cc::rec::event_kind::log, (level_), ::cc::rec::enable_bit_of(level_), \
                           (fmt_), nullptr, nullptr, 0, ::cc::rec::desc::variable_payload);                           \
        if (::cc::rec::is_recording(cc_rec_site_desc_))                                                               \
            ::cc::rec::impl::log_write(cc_rec_site_desc_, (fmt_)__VA_OPT__(, ) __VA_ARGS__);                          \
    } while (false)

/// The noisiest level, off by default — per-iteration detail nobody wants until they do.
#define CC_LOG_TRACE(...) CC_REC_IMPL_LOG(::cc::rec::level::trace, __VA_ARGS__)

/// Developer detail, off by default.
#define CC_LOG_DEBUG(...) CC_REC_IMPL_LOG(::cc::rec::level::debug, __VA_ARGS__)

/// What happened, for someone reading the log afterwards.
/// On by default.
#define CC_LOG_INFO(...) CC_REC_IMPL_LOG(::cc::rec::level::info, __VA_ARGS__)

/// Something is wrong but the program continues.
#define CC_LOG_WARNING(...) CC_REC_IMPL_LOG(::cc::rec::level::warning, __VA_ARGS__)

/// Something failed.
/// Captures a stack by default, which costs orders of magnitude more than the message.
#define CC_LOG_ERROR(...) CC_REC_IMPL_LOG(::cc::rec::level::error, __VA_ARGS__)
