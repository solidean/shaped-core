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

/// Whether T is a string literal: an array of CONST char, stored by address rather than copied.
///
/// The const is the discriminator, and it earns its keep.
/// A string literal is `char const[N]`; a buffer you formatted into must be `char[N]` to have been written at all, and
/// falls through to the inline path below where its bytes are copied.
/// So the case that actually comes up — `snprintf` into a local, then record it — is safe without anyone thinking
/// about it.
///
/// **The bytes must outlive the process**, because the event carries only their address.
/// Nothing reads the payload at the recording site: the actor drains the chunk later, a capture holds it for as long
/// as it lives, and serializing walks it later still — so a `char const buf[32]` local is a garbage read at every one
/// of those points, whatever it was NUL-terminated with.
/// That case is misuse rather than something the type system catches; pass `cc::string_view(buf)` to copy instead.
///
/// The extent is deliberately unused: length comes from the first NUL when a reader asks, so nothing has to store it.
///
/// Keyed on the type with its reference stripped, because `decltype` of a string literal is `char const(&)[N]` — a
/// literal is an lvalue, so decltype hands back a reference and a specialization on the array alone never matches.
template <class T>
inline constexpr bool is_literal_text = false;

template <size_t N>
inline constexpr bool is_literal_text<char const[N]> = true;

/// Anything else convertible to a string_view is recorded as its BYTES.
/// That includes `char const*`: a pointer variable may name a buffer that is gone by the time anything reads the
/// event, and recording the address would be technically defensible and never what was meant.
template <class T>
inline constexpr bool is_text_value = !is_literal_text<T> && std::is_convertible_v<T const&, cc::string_view>;

/// Whether T is recorded by copying a fixed number of bytes.
template <class T>
inline constexpr bool is_scalar_value
    = !is_text_value<T> && !is_literal_text<T>
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

/// The field descriptor for a literal: the address alone, since the bytes are in the binary.
inline constexpr rec::field literal_value_fields[] = {
    {.name = "value", .type = rec::type_code::cstring, .offset = 0, .size = 8},
};

/// Whether T's value occupies a fixed, compile-time-known number of bytes.
/// True for scalars, enums, pointers and literals; false only for text that has to be copied.
template <class T>
inline constexpr bool is_fixed_size_value = is_scalar_value<T> || is_literal_text<T>;

/// How many bytes T's value occupies, for the fixed-size cases.
///
/// A literal is eight whatever `sizeof(char const*)` happens to be: a payload slot holding a POINTER is eight bytes on
/// every target, so the wire layout does not follow the writer's architecture.
/// wasm32 is the target that makes that concrete, with four-byte pointers and a 64-bit file to write.
template <class T>
inline constexpr u16 fixed_value_bytes = is_literal_text<T> ? u16(sizeof(u64)) : u16(sizeof(value_storage_t<T>));

/// The one `value` field of a fixed-size payload, at a caller-chosen offset.
template <class T>
[[nodiscard]] constexpr rec::field fixed_value_field(u16 offset)
{
    auto type = rec::type_code::cstring;
    if constexpr (!is_literal_text<T>)
        type = std::is_pointer_v<std::remove_cvref_t<T>> ? rec::type_code::pointer
                                                         : rec::type_code_of<value_storage_t<T>>;

    return {.name = "value", .type = type, .offset = offset, .size = fixed_value_bytes<T>};
}

/// The layout of a runtime-named value: the value at offset zero, then the name after it.
///
/// **The value goes first precisely so both offsets stay compile-time constants.**
/// An offset lives in the descriptor, so at most one variable-length field can exist and it has to be last — which is
/// why the dynamic-name form takes a fixed-size value only.
/// Putting the name first would make the value's offset depend on how long the name turned out to be, and no generic
/// reader could find it.
template <class T>
inline constexpr rec::field named_value_fields[] = {
    fixed_value_field<T>(0),
    {.name = "name", .type = rec::type_code::inline_text, .offset = fixed_value_bytes<T>, .size = 4},
};

/// Writes `text` as an inline_text payload.
/// Out of line because it takes the reserve-and-fill path rather than a fixed-size copy, and truncates rather than
/// rotating when the chunk is nearly full.
void record_text_value(rec::desc const& d, cc::string_view text);

/// Writes a fixed-size value followed by an inline name, for a site whose name is not known until it runs.
void record_named_value(rec::desc const& d, void const* value, isize value_size, cc::string_view name);

/// The dynamic-name form of record_value, for the fixed-size types only.
template <class T>
CC_FORCE_INLINE void record_named(rec::desc const& d, cc::string_view name, T const& value)
{
    static_assert(is_fixed_size_value<T>,
                  "CC_RECORD_NAMED takes a scalar, an enum, a pointer, or a string literal — a runtime string VALUE "
                  "with a runtime NAME would need two variable-length fields, and a field's offset is a constant. "
                  "Format the two together and use CC_LOG_INFO, or name the site statically with CC_RECORD.");

    if constexpr (is_literal_text<T>)
    {
        // Widened to eight bytes, matching fixed_value_bytes and every other pointer-carrying slot.
        auto const address = u64(reinterpret_cast<uintptr_t>(static_cast<char const*>(value)));
        record_named_value(d, &address, isize(sizeof(address)), name);
    }
    else
    {
        auto const stored = value_storage_t<T>(value);
        record_named_value(d, &stored, isize(sizeof(stored)), name);
    }
}

/// Sends a value down the scalar, literal or text path, so one macro covers all three.
///
/// T is passed EXPLICITLY by the macro rather than deduced, because deduction strips exactly the thing the literal
/// path keys on: `char const[8]` deduces the same as `char[8]` through a `T const&` parameter.
template <class T>
CC_FORCE_INLINE void record_value(rec::desc const& d, T const& value)
{
    static_assert(is_scalar_value<T> || is_text_value<T> || is_literal_text<T>,
                  "CC_RECORD takes a scalar, an enum, a pointer, or something convertible to a cc::string_view");

    if constexpr (is_scalar_value<T>)
        rec::record_event(d, value_storage_t<T>(value));
    else if constexpr (is_literal_text<T>)
        // The address, widened to eight bytes so the slot is the same size on every target; the bytes stay in the binary.
        rec::record_event(d, u64(reinterpret_cast<uintptr_t>(static_cast<char const*>(value))));
    else
        record_text_value(d, cc::string_view(value));
}

/// The payload size a value site declares, for a consumer that wants to know before it reads.
template <class T>
inline constexpr u32 value_payload_size = is_scalar_value<T> ? u32(sizeof(value_storage_t<T>))
                                        : is_literal_text<T> ? u32(sizeof(char const*))
                                                             : rec::desc::variable_payload;

/// Which field layout a value site declares.
template <class T>
inline constexpr rec::field const* value_fields_of = is_scalar_value<T> ? scalar_value_fields<T>
                                                   : is_literal_text<T> ? literal_value_fields
                                                                        : text_value_fields;

/// What a dynamic-name site declares as its payload size: fixed value plus a name of unknown length.
template <class T>
inline constexpr u32 named_value_payload_size = rec::desc::variable_payload;
} // namespace cc::rec::impl

/// Records that this point was reached, with no data attached.
///
/// The cheapest useful annotation there is.
/// Reach for it in a fallback branch or an edge case you are not sure is ever taken — "was this ever hit" is a question
/// a marker answers and a debugger does not.
#define CC_RECORD_MARK(name_) CC_RECORD_EVENT(::cc::rec::event_kind::marker, ::cc::rec::category::values, name_)

/// Records one value under `name_`.
///
/// Scalars, enums and pointers are copied inline, and so is anything convertible to a cc::string_view.
/// A string LITERAL — anything of type `char const[N]` — is stored as its address instead, so the bytes cost the
/// stream nothing; see `is_literal_text` for the static-lifetime rule that comes with it.
///
/// `name_` must be a compile-time constant, since it lives in the site's descriptor rather than in the stream.
/// CC_RECORD_NAMED is the form for a name only known at runtime.
#define CC_RECORD(name_, value_)                                                                    \
    do                                                                                              \
    {                                                                                               \
        using cc_rec_value_t = ::std::remove_reference_t<decltype(value_)>;                         \
        CC_REC_DEFINE_DESC(cc_rec_site_desc_, ::cc::rec::event_kind::value, ::cc::rec::level::info, \
                           ::cc::rec::enable_bit_of(::cc::rec::category::values), (name_), nullptr, \
                           ::cc::rec::impl::value_fields_of<cc_rec_value_t>, 1,                     \
                           ::cc::rec::impl::value_payload_size<cc_rec_value_t>);                    \
        ::cc::rec::impl::record_value<cc_rec_value_t>(cc_rec_site_desc_, (value_));                 \
    } while (false)

/// Records one value under a name only known at runtime.
///
///   CC_RECORD_NAMED(counter.name(), counter.value());
///
/// **Prefer CC_RECORD wherever the name is a constant.**
/// A static name costs the stream nothing — it lives in the descriptor — where this one is copied into every event,
/// and a query keyed on it has to read the payload rather than compare a pointer.
///
/// The value must be fixed-size: a scalar, an enum, a pointer or a string literal.
/// A runtime string value alongside a runtime name would need two variable-length fields, which a descriptor's
/// constant offsets cannot express; format the two together and log them instead.
///
/// The site still has a descriptor and still gates on its domain, so a disabled one costs exactly what any other does.
#define CC_RECORD_NAMED(name_, value_)                                                              \
    do                                                                                              \
    {                                                                                               \
        using cc_rec_value_t = ::std::remove_reference_t<decltype(value_)>;                         \
        CC_REC_DEFINE_DESC(cc_rec_site_desc_, ::cc::rec::event_kind::value, ::cc::rec::level::info, \
                           ::cc::rec::enable_bit_of(::cc::rec::category::values), "", nullptr,      \
                           ::cc::rec::impl::named_value_fields<cc_rec_value_t>, 2,                  \
                           ::cc::rec::impl::named_value_payload_size<cc_rec_value_t>);              \
        ::cc::rec::impl::record_named<cc_rec_value_t>(cc_rec_site_desc_, (name_), (value_));        \
    } while (false)
