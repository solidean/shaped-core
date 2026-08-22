#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/record/chunk.hh>
#include <clean-core/record/value.hh>

// Recording bytes BY PIN rather than by copy.
//
// `CC_RECORD("frame", some_pinned_data)` writes an address and a size, and hands the chunk a reference that keeps the
// bytes alive for exactly as long as the chunk is.
// So a megabyte of mesh data costs the stream sixteen bytes, and a debugger — or a test — can look at what the code
// actually had rather than at a summary somebody remembered to write.
//
// **Opt-in per type, through cc::rec::pinnable_traits.**
// Pinning is a promise about lifetime and immutability that only the type's author can make, so nothing is pinnable by
// accident: a type says so, or it is copied like anything else.
//
// Its own header rather than part of value.hh, because pinned_data reaches <memory> and value.hh is included by every
// site that records anything at all.

/// Opt a type in to being recorded by pin.
///
/// Specialize with a `static cc::pinned_data<byte const> bytes_of(T const&)` to make `CC_RECORD_PINNED` pin a T instead
/// of copying it.
/// The bytes must not change for as long as the recording holding them lives — that is the promise, and nothing checks
/// it.
template <class T>
struct cc::rec::pinnable_traits
{
    static constexpr bool is_pinnable = false;
};

/// Every `cc::pinned_data<T>` is pinnable, which is the whole point of the type.
template <class T>
struct cc::rec::pinnable_traits<cc::pinned_data<T>>
{
    static constexpr bool is_pinnable = true;

    [[nodiscard]] static cc::pinned_data<cc::byte const> bytes_of(cc::pinned_data<T> const& d) { return d.as_bytes(); }
};

namespace cc::rec::impl
{
/// Whether T opted in to being recorded by pin.
template <class T>
inline constexpr bool is_pinnable_value = rec::pinnable_traits<std::remove_cvref_t<T>>::is_pinnable;

/// The layout of a pinned payload: where the bytes are, and how many.
///
/// The address is what makes this cheap and what makes it process-local, so it is `pinned_bytes` rather than
/// `pointer` — a reader must be told that the bytes behind it are real and readable, and that a serialized recording
/// has moved them.
inline constexpr rec::field pinned_value_fields[] = {
    {.name = "value", .type = rec::type_code::pinned_bytes, .offset = 0, .size = 8},
    {.name = "size", .type = rec::type_code::u64_, .offset = 8, .size = 8},
};

/// What a pinned payload holds.
///
/// `data` is a u64 rather than a pointer, like every other payload slot that carries one: eight bytes on every target,
/// so the wire layout does not follow the writer's pointer width.
struct pinned_payload
{
    u64 data = 0;
    u64 size = 0;
};
static_assert(sizeof(pinned_payload) == 16, "pinned_value_fields describes this layout");

/// Takes a reference to `owner` on the recording thread's current chunk, and writes the event that points into it.
///
/// Returns false when the chunk would not take the pin, at which point the caller has recorded nothing and should say
/// so rather than write an address nothing is keeping alive.
bool record_pinned_bytes(rec::desc const& d, cc::pinned_data<byte const> const& bytes);

/// Records a pinnable value by pin, falling back to a marker when the pin could not be taken.
template <class T>
void record_pinned(rec::desc const& d, T const& value)
{
    if (!rec::is_recording(d))
        return;

    record_pinned_bytes(d, rec::pinnable_traits<std::remove_cvref_t<T>>::bytes_of(value));
}
} // namespace cc::rec::impl

/// Records `value_` by PIN: the event carries an address and a size, and the chunk keeps the bytes alive.
///
///   CC_RECORD_PINNED("frame.depth", depth_buffer); // a cc::pinned_data<f32 const>
///
/// **The bytes must not change while any recording holding them is alive.**
/// Nothing copies them, so a mutation after the fact rewrites history rather than being recorded as a second value.
///
/// Serializing copies them into the file's blob section, so a loaded recording reads its own storage — but a live
/// recording's `field_as_bytes` points at the original, which is what makes this free.
#define CC_RECORD_PINNED(name_, value_)                                                                                  \
    do                                                                                                                   \
    {                                                                                                                    \
        using cc_rec_value_t = ::std::remove_reference_t<decltype(value_)>;                                              \
        static_assert(::cc::rec::impl::is_pinnable_value<cc_rec_value_t>, "CC_RECORD_PINNED needs a type that opted "    \
                                                                          "in through cc::rec::pinnable_traits");        \
        CC_REC_DEFINE_DESC(cc_rec_site_desc_, ::cc::rec::event_kind::value, ::cc::rec::level::info,                      \
                           ::cc::rec::enable_bit_of(::cc::rec::category::values), (name_), nullptr,                      \
                           ::cc::rec::impl::pinned_value_fields, 2, ::cc::u32(sizeof(::cc::rec::impl::pinned_payload))); \
        ::cc::rec::impl::record_pinned<cc_rec_value_t>(cc_rec_site_desc_, (value_));                                     \
    } while (false)
