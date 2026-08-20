#pragma once

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
    CC_FORCE_INLINE scope_guard(rec::desc const& begin_desc, rec::desc const& end_desc) : _end(&end_desc)
    {
        if (!rec::is_recording(begin_desc))
        {
            _end = nullptr;
            return;
        }

        auto& w = t_writer;
        rec::record_event(begin_desc, w.scope_depth);
        ++w.scope_depth;
    }

    CC_FORCE_INLINE ~scope_guard()
    {
        if (_end == nullptr)
            return;

        auto& w = t_writer;
        --w.scope_depth;
        rec::record_event(*_end, w.scope_depth);
    }

    scope_guard(scope_guard const&) = delete;
    scope_guard& operator=(scope_guard const&) = delete;

private:
    /// Null when the site was disabled at entry, which is also what keeps the pair balanced across a reconfiguration
    /// that lands mid-scope.
    rec::desc const* _end;
};
} // namespace cc::rec::impl

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
    auto const cc_rec_scope_ = ::cc::rec::impl::scope_guard(cc_rec_scope_begin_, cc_rec_scope_end_)

/// Opens a scope without a matching block, for a span whose ends are in different functions.
/// The caller owes a CC_RECORD_SCOPE_END on the same thread, and an unbalanced pair produces a wrong trace rather than
/// a diagnostic — prefer CC_RECORD_SCOPE wherever the span fits a block.
#define CC_RECORD_SCOPE_BEGIN(name_)                                                                      \
    do                                                                                                    \
    {                                                                                                     \
        CC_REC_DEFINE_DESC(cc_rec_site_desc_, ::cc::rec::event_kind::scope_begin, ::cc::rec::level::info, \
                           ::cc::rec::enable_bit_of(::cc::rec::category::profiling), (name_), nullptr,    \
                           ::cc::rec::impl::scope_fields, 1, 4);                                          \
        if (::cc::rec::is_recording(cc_rec_site_desc_))                                                   \
        {                                                                                                 \
            ::cc::rec::record_event(cc_rec_site_desc_, ::cc::rec::impl::t_writer.scope_depth);            \
            ++::cc::rec::impl::t_writer.scope_depth;                                                      \
        }                                                                                                 \
    } while (false)

/// Closes the scope CC_RECORD_SCOPE_BEGIN opened.
#define CC_RECORD_SCOPE_END(name_)                                                                      \
    do                                                                                                  \
    {                                                                                                   \
        CC_REC_DEFINE_DESC(cc_rec_site_desc_, ::cc::rec::event_kind::scope_end, ::cc::rec::level::info, \
                           ::cc::rec::enable_bit_of(::cc::rec::category::profiling), (name_), nullptr,  \
                           ::cc::rec::impl::scope_fields, 1, 4);                                        \
        if (::cc::rec::is_recording(cc_rec_site_desc_))                                                 \
        {                                                                                               \
            --::cc::rec::impl::t_writer.scope_depth;                                                    \
            ::cc::rec::record_event(cc_rec_site_desc_, ::cc::rec::impl::t_writer.scope_depth);          \
        }                                                                                               \
    } while (false)
