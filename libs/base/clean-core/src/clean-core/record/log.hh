#pragma once

#include <clean-core/record/record.hh>
#include <clean-core/string/format.hh>

#include <type_traits>

// CC_LOG_* — messages, formatted on the calling thread and written straight into the stream.
//
// A message with no arguments costs the stream NOTHING: the text lives in the site's descriptor, so the event is a
// header and no payload at all.
// A message with arguments is formatted directly into the chunk's remaining space, so there is no temporary buffer,
// no allocation and no copy — cc::format_to writes where the bytes will stay.
//
// Which domain a message belongs to comes from `cc_rec_domain()`, so a site never names one.
// Levels gate per domain, and a domain can also be told to capture a stack or break into the debugger at a level.

namespace cc::rec::impl
{
/// How much of a chunk one formatted message may claim.
/// Generous, because the alternative to truncating is dropping, and a truncated message is still evidence.
inline constexpr isize log_max_payload = 4096;

/// Writes a message whose text is already in the descriptor, so the event carries no payload.
void log_write(rec::desc const& d, cc::format_string<> fmt);

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

/// Formats a message into the chunk and publishes it.
/// A message longer than what the chunk can take is truncated and flagged, never dropped.
template <class... Args>
    requires(sizeof...(Args) > 0)
void log_write(rec::desc const& d, cc::format_string<std::type_identity_t<Args>...> fmt, Args&&... args)
{
    auto writer = rec::open_event(d, log_max_payload);
    if (writer.is_open())
    {
        auto const out = writer.payload();
        auto const written = cc::format_to(cc::span<char>(reinterpret_cast<char*>(out.data()), out.size()), fmt,
                                           cc::forward<Args>(args)...);
        writer.commit(written);
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
