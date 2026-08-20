#pragma once

#include <clean-core/record/record.hh>
#include <clean-core/string/string_view.hh>

#include <type_traits>

// CC_RECORD and CC_RECORD_MARK — attaching data to a point in the stream.
//
// A marker answers "did this code run", which is the cheapest useful thing you can record.
// It is the one to reach for in a fallback branch or an edge case you are not sure is ever taken.
// A value answers "with what", so a scope's duration can later be correlated against the size it was given.
//
// **The value codec is deliberately capped for now**: scalars, enums, pointers and text.
// Structured multi-field payloads are what desc::fields already describes, and nothing needs them yet.

namespace cc::rec
{
/// How a T is laid out in a payload, for a consumer that has never heard of it.
///
/// The primary template is `none`.
/// That is what turns an unsupported type into a compile error at the macro rather than an unreadable event.
template <class T>
inline constexpr rec::type_code type_code_of = rec::type_code::none;

// clang-format off
template <> inline constexpr rec::type_code type_code_of<bool> = rec::type_code::boolean;
template <> inline constexpr rec::type_code type_code_of<i8>   = rec::type_code::i8_;
template <> inline constexpr rec::type_code type_code_of<i16>  = rec::type_code::i16_;
template <> inline constexpr rec::type_code type_code_of<i32>  = rec::type_code::i32_;
template <> inline constexpr rec::type_code type_code_of<i64>  = rec::type_code::i64_;
template <> inline constexpr rec::type_code type_code_of<u8>   = rec::type_code::u8_;
template <> inline constexpr rec::type_code type_code_of<u16>  = rec::type_code::u16_;
template <> inline constexpr rec::type_code type_code_of<u32>  = rec::type_code::u32_;
template <> inline constexpr rec::type_code type_code_of<u64>  = rec::type_code::u64_;
template <> inline constexpr rec::type_code type_code_of<f32>  = rec::type_code::f32_;
template <> inline constexpr rec::type_code type_code_of<f64>  = rec::type_code::f64_;
// clang-format on
} // namespace cc::rec

namespace cc::rec::impl
{
/// What a recorded value of type T is stored as.
/// An enum collapses onto its underlying type and a pointer onto an opaque address, so a caller never has to think
/// about which spelling it happened to hold.
template <class T>
struct value_storage
{
    using type = std::remove_cvref_t<T>;
};

template <class T>
    requires std::is_enum_v<std::remove_cvref_t<T>>
struct value_storage<T>
{
    using type = std::underlying_type_t<std::remove_cvref_t<T>>;
};

template <class T>
    requires std::is_pointer_v<std::remove_cvref_t<T>>
struct value_storage<T>
{
    using type = void const*;
};

template <class T>
using value_storage_t = typename value_storage<T>::type;

/// Anything convertible to a string_view is recorded as its BYTES, never as an address.
/// That includes `char const*`, where recording the pointer would be technically defensible and never what was meant.
template <class T>
inline constexpr bool is_text_value = std::is_convertible_v<T const&, cc::string_view>;

/// Whether T is recorded by copying a fixed number of bytes.
template <class T>
inline constexpr bool is_scalar_value
    = !is_text_value<T>
   && (rec::type_code_of<value_storage_t<T>> != rec::type_code::none || std::is_pointer_v<std::remove_cvref_t<T>>);

/// The field descriptor for a one-scalar payload.
template <class T>
inline constexpr rec::field scalar_value_fields[] = {
    {.name = "value",
     .type = std::is_pointer_v<std::remove_cvref_t<T>> ? rec::type_code::pointer : rec::type_code_of<value_storage_t<T>>,
     .offset = 0,
     .size = u16(sizeof(value_storage_t<T>))},
};

/// The field descriptor for a one-text payload: a u32 length, then that many bytes.
inline constexpr rec::field text_value_fields[] = {
    {.name = "value", .type = rec::type_code::inline_text, .offset = 0, .size = 4},
};

/// Writes `text` as an inline_text payload.
/// Out of line because it takes the reserve-and-fill path rather than a fixed-size copy, and truncates rather than
/// rotating when the chunk is nearly full.
void record_text_value(rec::desc const& d, cc::string_view text);

/// Sends a value down the scalar or the text path, so one macro covers both.
template <class T>
CC_FORCE_INLINE void record_value(rec::desc const& d, T const& value)
{
    static_assert(is_scalar_value<T> || is_text_value<T>, "CC_RECORD takes a scalar, an enum, a pointer, or something "
                                                          "convertible to a cc::string_view");

    if constexpr (is_scalar_value<T>)
        rec::record_event(d, value_storage_t<T>(value));
    else
        record_text_value(d, cc::string_view(value));
}

/// The payload size a value site declares, for a consumer that wants to know before it reads.
template <class T>
inline constexpr u32 value_payload_size
    = is_scalar_value<T> ? u32(sizeof(value_storage_t<T>)) : rec::desc::variable_payload;
} // namespace cc::rec::impl

/// Records that this point was reached, with no data attached.
///
/// The cheapest useful annotation there is.
/// Reach for it in a fallback branch or an edge case you are not sure is ever taken — "was this ever hit" is a question
/// a marker answers and a debugger does not.
#define CC_RECORD_MARK(name_) CC_RECORD_EVENT(::cc::rec::event_kind::marker, ::cc::rec::category::values, name_)

/// Records one value under `name_`.
///
/// Scalars, enums and pointers are copied inline; anything convertible to a cc::string_view has its bytes copied.
/// `name_` must be a string literal, since it lives in the site's descriptor rather than in the stream.
#define CC_RECORD(name_, value_)                                                                    \
    do                                                                                              \
    {                                                                                               \
        using cc_rec_value_t = decltype(value_);                                                    \
        CC_REC_DEFINE_DESC(cc_rec_site_desc_, ::cc::rec::event_kind::value, ::cc::rec::level::info, \
                           ::cc::rec::enable_bit_of(::cc::rec::category::values), (name_), nullptr, \
                           ::cc::rec::impl::is_scalar_value<cc_rec_value_t>                         \
                               ? ::cc::rec::impl::scalar_value_fields<cc_rec_value_t>               \
                               : ::cc::rec::impl::text_value_fields,                                \
                           1, ::cc::rec::impl::value_payload_size<cc_rec_value_t>);                 \
        ::cc::rec::impl::record_value(cc_rec_site_desc_, (value_));                                 \
    } while (false)
