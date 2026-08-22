#pragma once

#include <clean-core/platform/intrinsics.hh>
#include <clean-core/record/record.hh>

// CC_RECORD_SCOPE — a timed span on one thread, and the thing a flame graph is built out of.
//
// **A scope is strictly thread-local and strictly non-suspending.**
// It opens and closes on the same thread, at the same nesting depth, and a scope that crosses a `co_await` is a lie:
// the work stopped, the thread went elsewhere, and the span it reports never happened.
// cc::async's frame driver asserts on exactly that, so the mistake is caught rather than reported as a wrong number.
//
// For work that suspends, CC_RECORD_ASYNC_SCOPE in record/async_scope.hh is the primitive — it rides the ambient
// chain, follows the work to whichever worker picks it up, and nests logically rather than as a stack frame.
//
// Every scope event carries the depth it was recorded at.
// One byte of payload buys best-effort re-nesting of a stream that lost its middle, which is exactly the stream a
// crash dump or a decimated ring buffer hands you.

namespace cc::rec::impl
{
/// The depth field both scope events carry.
inline constexpr rec::field scope_fields[] = {
    {.name = "depth", .type = rec::type_code::u32_, .offset = 0, .size = 4},
};

/// Opens a scope on construction and closes it on destruction.
///
/// Both descriptors are the site's own, so the pair is matched by identity rather than by name — two scopes sharing a
/// name still nest correctly.
struct scope_guard
{
    /// `condition` is the caller's own gate, evaluated once here and never consulted again.
    /// Tested BEFORE the site's, so a false one costs a branch and not the domain's load.
    CC_FORCE_INLINE scope_guard(rec::desc const& begin_desc, rec::desc const& end_desc, bool condition)
      : _end(&end_desc)
    {
        if (!condition || !rec::is_recording(begin_desc))
        {
            _end = nullptr;
            return;
        }

        auto& w = t_writer;
        rec::record_event(begin_desc, w.scope_depth);

        // Remembered for the next chunk's preamble, and only for the outermost few — see writer_tls.
        if (w.scope_depth < rec::impl::named_scope_capacity)
            w.scope_descs[w.scope_depth] = &begin_desc;
        ++w.scope_depth;

        // The enclosing scope's frame is saved here rather than recomputed, so leaving restores it exactly even where
        // this one was inlined into a caller that already had a scope open.
        _enclosing_frame = w.scope_frame;
        w.scope_frame = cc::impl::current_frame_address();
    }

    CC_FORCE_INLINE ~scope_guard()
    {
        if (_end == nullptr)
            return;

        auto& w = t_writer;
        w.scope_frame = _enclosing_frame;
        --w.scope_depth;
        rec::record_event(*_end, w.scope_depth);
    }

    scope_guard(scope_guard const&) = delete;
    scope_guard& operator=(scope_guard const&) = delete;

private:
    /// What w.scope_frame held before this scope, restored on the way out.
    void* _enclosing_frame = nullptr;

    /// Null when the site was disabled at entry, which is also what keeps the pair balanced across a reconfiguration
    /// that lands mid-scope.
    rec::desc const* _end;
};

/// Pops the innermost open scope for a CC_RECORD_SCOPE_END, reporting whether there was one to pop.
///
/// **`scope_depth` is unsigned**, so an END with no matching BEGIN would wrap it to four billion and leave every
/// later scope on that thread misplaced — for the rest of the thread's life, not just for the frame that slipped.
/// Refusing is what bounds the damage of an unmatched pair to the one span that went missing.
///
/// Deliberately not an assert: the same "nothing open" state is what a domain enabled BETWEEN the begin and the end
/// legitimately produces, and scope_guard already absorbs that case the same way through its null `_end`.
[[nodiscard]] CC_FORCE_INLINE bool pop_unmatched_scope()
{
    auto& w = t_writer;
    if (w.scope_depth == 0)
        return false;

    --w.scope_depth;
    return true;
}
} // namespace cc::rec::impl

namespace cc::rec
{
/// The stack frame that opened the innermost profiling scope on this thread, or null when none is open.
///
/// This is what a stack capture passes as its `stop_frame`: everything below that frame is already named by the scope
/// stack, so capturing it again spends both time and payload on a fact the stream already carries.
[[nodiscard]] CC_FORCE_INLINE void* current_scope_frame()
{
    return rec::impl::t_writer.scope_frame;
}
} // namespace cc::rec

/// Defines the begin/end descriptor pair for one scope site.
#define CC_REC_IMPL_SCOPE_DESCS(name_)                                                                  \
    CC_REC_DEFINE_DESC(cc_rec_scope_begin_, ::cc::rec::event_kind::scope_begin, ::cc::rec::level::info, \
                       ::cc::rec::enable_bit_of(::cc::rec::category::profiling), (name_), nullptr,      \
                       ::cc::rec::impl::scope_fields, 1, 4);                                            \
    CC_REC_DEFINE_DESC(cc_rec_scope_end_, ::cc::rec::event_kind::scope_end, ::cc::rec::level::info,     \
                       ::cc::rec::enable_bit_of(::cc::rec::category::profiling), (name_), nullptr,      \
                       ::cc::rec::impl::scope_fields, 1, 4)

/// Picks the scope's name: the argument if there is one, the enclosing function otherwise.
#define CC_REC_IMPL_SCOPE_NAME(...) CC_REC_IMPL_FIRST_OF(__VA_ARGS__ __VA_OPT__(, ) CC_PRETTY_FUNC)
#define CC_REC_IMPL_FIRST_OF(first_, ...) first_

/// Times the enclosing block, named either explicitly or after the function it sits in.
///
///   CC_RECORD_SCOPE();              // named after the enclosing function
///   CC_RECORD_SCOPE("upload-pass"); // named explicitly
///
/// Must not cross a `co_await` — see the header comment, and CC_RECORD_ASYNC_SCOPE for work that suspends.
#define CC_RECORD_SCOPE(...)                                      \
    CC_REC_IMPL_SCOPE_DESCS(CC_REC_IMPL_SCOPE_NAME(__VA_ARGS__)); \
    auto const cc_rec_scope_ = ::cc::rec::impl::scope_guard(cc_rec_scope_begin_, cc_rec_scope_end_, true)

/// Times the enclosing block only when `cond_` holds, named either explicitly or after the function it sits in.
///
///   CC_RECORD_SCOPE_IF(bytes.size() >= json_scope_threshold);
///   CC_RECORD_SCOPE_IF(bytes.size() >= json_scope_threshold, "json-parse");
///
/// **The condition is evaluated once, at entry**, and does not affect whether the scope closes: a scope that opened
/// closes, whatever the condition would say by then.
///
/// This is for sites where the EVENT RATE is the problem, not the duration — the sampler finds slow things on its
/// own, and needs no help from a scope to do it.
/// A JSON parse of a twelve-byte value called a million times a frame would bury the stream; the same call on a forty
/// megabyte document is exactly the span you want.
/// Give the threshold a name rather than writing a number here.
#define CC_RECORD_SCOPE_IF(cond_, ...)                            \
    CC_REC_IMPL_SCOPE_DESCS(CC_REC_IMPL_SCOPE_NAME(__VA_ARGS__)); \
    auto const cc_rec_scope_ = ::cc::rec::impl::scope_guard(cc_rec_scope_begin_, cc_rec_scope_end_, bool(cond_))

/// Opens a scope without a matching block, for a span whose ends are in different functions.
///
/// The caller owes a CC_RECORD_SCOPE_END on the same thread, and an unbalanced pair produces a wrong trace rather than
/// a diagnostic — prefer CC_RECORD_SCOPE wherever the span fits a block.
/// A BEGIN whose END is never reached costs exactly the span it opened: the depth is only ever popped by an END that
/// finds something open, so it cannot go negative and poison every later scope on the thread.
/// That bounds the mistake; it does not make it correct, and the span will still be wrong.
#define CC_RECORD_SCOPE_BEGIN(name_)                                                                               \
    do                                                                                                             \
    {                                                                                                              \
        CC_REC_DEFINE_DESC(cc_rec_site_desc_, ::cc::rec::event_kind::scope_begin, ::cc::rec::level::info,          \
                           ::cc::rec::enable_bit_of(::cc::rec::category::profiling), (name_), nullptr,             \
                           ::cc::rec::impl::scope_fields, 1, 4);                                                   \
        if (::cc::rec::is_recording(cc_rec_site_desc_))                                                            \
        {                                                                                                          \
            ::cc::rec::record_event(cc_rec_site_desc_, ::cc::rec::impl::t_writer.scope_depth);                     \
            if (::cc::rec::impl::t_writer.scope_depth < ::cc::rec::impl::named_scope_capacity)                     \
                ::cc::rec::impl::t_writer.scope_descs[::cc::rec::impl::t_writer.scope_depth] = &cc_rec_site_desc_; \
            ++::cc::rec::impl::t_writer.scope_depth;                                                               \
        }                                                                                                          \
    } while (false)

/// Closes the scope CC_RECORD_SCOPE_BEGIN opened.
#define CC_RECORD_SCOPE_END(name_)                                                                      \
    do                                                                                                  \
    {                                                                                                   \
        CC_REC_DEFINE_DESC(cc_rec_site_desc_, ::cc::rec::event_kind::scope_end, ::cc::rec::level::info, \
                           ::cc::rec::enable_bit_of(::cc::rec::category::profiling), (name_), nullptr,  \
                           ::cc::rec::impl::scope_fields, 1, 4);                                        \
        if (::cc::rec::is_recording(cc_rec_site_desc_) && ::cc::rec::impl::pop_unmatched_scope())       \
            ::cc::rec::record_event(cc_rec_site_desc_, ::cc::rec::impl::t_writer.scope_depth);          \
    } while (false)
