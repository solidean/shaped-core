#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/enum_traits.hh>
#include <clean-core/common/hash.hh>
#include <clean-core/fwd.hh>
#include <clean-core/math/bit.hh>

/// A set of flags drawn from one enum, packed into the storage the enum declared.
///
/// A plain enum needs nothing special — CC_FLAG_ENUM_INDEXED gives each of its values a bit of its own:
///
///     namespace app { enum class shape { visible, selected, locked }; }
///     CC_FLAG_ENUM_INDEXED(app, shape, u32);      // visible takes bit 0, selected bit 1, locked bit 2
///
/// An enum whose values already ARE bit patterns says so instead, and may then name several bits at once:
///
///     namespace app { enum class usage : u32 { vertex = 1u << 0, index = 1u << 1, both = 0b11 }; }
///     CC_FLAG_ENUM_BITMASK(app, usage, u32);
///
/// Either way the set operations are the same, and a single flag converts implicitly, so every query and parameter
/// below takes one flag or a whole set:
///
///     auto f = app::shape::visible | app::shape::selected;
///     f.has(app::shape::locked);                  // false
///     f = f.without(app::shape::selected);
///
/// There is deliberately no operator~: every use of a complement here is set subtraction, and without() does that
/// without anyone having to declare which bits the enum actually defines.
template <class EnumT>
struct cc::flags
{
    static_assert(cc::flag_enum<EnumT>,
                  "EnumT must opt in with CC_FLAG_ENUM_INDEXED or CC_FLAG_ENUM_BITMASK(namespace, EnumT, storage) at "
                  "global scope");

    using enum_type = EnumT;
    using storage_type = typename cc::custom::enum_traits<EnumT>::flag_storage_type;

    /// bit_index or bit_mask — how a value of EnumT names its bits.
    /// See cc::flag_encoding.
    static constexpr cc::flag_encoding encoding = cc::custom::enum_traits<EnumT>::flag_encoding;

    static_assert(std::is_unsigned_v<storage_type>, "flag storage must be an unsigned integer");

    /// The set as raw bits.
    /// Public because that is what makes cc::flags a structural type, so a flag set can serve as a non-type template parameter.
    /// Prefer the named operations; this is for interop and for bit patterns the enum defines itself.
    ///
    /// The initializer is load-bearing: it is what makes a default-initialized `cc::flags<E> f;` the EMPTY set rather than indeterminate bits.
    /// It costs trivial default construction, which nothing in clean-core requires.
    /// Every triviality requirement here is trivial copyability and/or destructibility, and both survive.
    storage_type bits = 0;

    /// A flag value as storage bits — the one place the two encodings differ.
    ///
    /// The declared storage may be narrower than the enum's underlying type, which is the whole point of declaring it,
    /// so a value that does not fit must be caught here rather than silently losing its high bits.
    /// A negative value fails the same check: it converts to a huge unsigned one, which fits neither encoding.
    [[nodiscard]] static constexpr storage_type bits_of(EnumT v)
    {
        using underlying_t = std::underlying_type_t<EnumT>;
        auto const raw = u64(std::make_unsigned_t<underlying_t>(static_cast<underlying_t>(v)));

        if constexpr (encoding == cc::flag_encoding::bit_index)
        {
            CC_ASSERT(raw < u64(8 * sizeof(storage_type)), "flag index does not fit the storage declared by "
                                                           "CC_FLAG_ENUM_INDEXED");
            return storage_type(storage_type(1) << raw);
        }
        else
        {
            CC_ASSERT(raw <= u64(storage_type(~storage_type(0))), "flag value does not fit the storage declared by "
                                                                  "CC_FLAG_ENUM_BITMASK");
            return storage_type(raw);
        }
    }

    // construction
public:
    constexpr flags() = default;

    /// Implicit on purpose: a single flag IS a one-element set, and everything below should accept one where it takes a set.
    constexpr flags(EnumT v) : bits(bits_of(v)) {}

    /// Two or more flags at once; the one-flag case is the constructor above.
    template <class... RestT>
        requires(std::is_same_v<RestT, EnumT> && ...)
    constexpr flags(EnumT a, EnumT b, RestT... rest)
    {
        set(a);
        set(b);
        (set(rest), ...);
    }

    [[nodiscard]] static constexpr flags create_from_bits(storage_type b)
    {
        auto r = flags();
        r.bits = b;
        return r;
    }

    // queries
public:
    /// every bit of v is set.
    /// Under bit_index that is a single-bit test; under bit_mask it is a subset test whenever v names more than one bit.
    [[nodiscard]] constexpr bool has(EnumT v) const { return (bits & bits_of(v)) == bits_of(v); }

    [[nodiscard]] constexpr bool has_any(flags f) const { return (bits & f.bits) != 0; }
    [[nodiscard]] constexpr bool has_all(flags f) const { return (bits & f.bits) == f.bits; }
    [[nodiscard]] constexpr bool is_empty() const { return bits == 0; }

    /// Number of set BITS.
    /// That is the number of set flags only where every enumerator is a single bit — one naming several counts as each of them.
    [[nodiscard]] constexpr i32 set_bit_count() const { return i32(cc::popcount(bits)); }

    /// this set with every bit of f cleared.
    [[nodiscard]] constexpr flags without(flags f) const
    {
        return create_from_bits(storage_type(bits & storage_type(~f.bits)));
    }

    // modifiers
public:
    constexpr void set(EnumT v) { bits = storage_type(bits | bits_of(v)); }
    constexpr void set(EnumT v, bool on) { on ? set(v) : remove(v); }
    constexpr void remove(EnumT v) { bits = storage_type(bits & storage_type(~bits_of(v))); }
    constexpr void toggle(EnumT v) { bits = storage_type(bits ^ bits_of(v)); }
    constexpr void clear() { bits = 0; }

    // set algebra
public:
    [[nodiscard]] friend constexpr flags operator|(flags a, flags b)
    {
        return create_from_bits(storage_type(a.bits | b.bits));
    }
    [[nodiscard]] friend constexpr flags operator&(flags a, flags b)
    {
        return create_from_bits(storage_type(a.bits & b.bits));
    }
    [[nodiscard]] friend constexpr flags operator^(flags a, flags b)
    {
        return create_from_bits(storage_type(a.bits ^ b.bits));
    }

    constexpr flags& operator|=(flags b) { return *this = *this | b; }
    constexpr flags& operator&=(flags b) { return *this = *this & b; }
    constexpr flags& operator^=(flags b) { return *this = *this ^ b; }

    /// There is no ordering on purpose: `<` would read as a subset test and compare bit patterns instead.
    /// has_all is the subset test.
    [[nodiscard]] friend constexpr bool operator==(flags const&, flags const&) = default;

    [[nodiscard]] friend constexpr u64 hash(flags const& f) { return cc::make_hash(f.bits); }
};

/// Shared expansion of the two opt-in macros below; reach for one of those, never this.
#define CC_IMPL_FLAG_ENUM(Namespace, EnumType, FlagStorageType, Encoding)           \
    template <>                                                                     \
    struct cc::custom::enum_traits<Namespace::EnumType>                             \
    {                                                                               \
        static constexpr bool is_flag_enum = true;                                  \
        static constexpr ::cc::flag_encoding flag_encoding = Encoding;              \
        using flag_storage_type = FlagStorageType;                                  \
    };                                                                              \
    namespace Namespace                                                             \
    {                                                                               \
    [[nodiscard]] constexpr ::cc::flags<EnumType> operator|(EnumType a, EnumType b) \
    {                                                                               \
        return ::cc::flags<EnumType>(a) | ::cc::flags<EnumType>(b);                 \
    }                                                                               \
    [[nodiscard]] constexpr ::cc::flags<EnumType> operator&(EnumType a, EnumType b) \
    {                                                                               \
        return ::cc::flags<EnumType>(a) & ::cc::flags<EnumType>(b);                 \
    }                                                                               \
    [[nodiscard]] constexpr ::cc::flags<EnumType> operator^(EnumType a, EnumType b) \
    {                                                                               \
        return ::cc::flags<EnumType>(a) ^ ::cc::flags<EnumType>(b);                 \
    }                                                                               \
    }                                                                               \
    static_assert(true, "CC_FLAG_ENUM_* wants a trailing semicolon")

/// Declare a PLAIN enum a flag enum: each value gets a bit of its own, `e` taking bit `e`.
/// This is the one to reach for when the enum was not written with bit patterns in mind.
///
///     namespace app { enum class shape { visible, selected, locked }; }
///     CC_FLAG_ENUM_INDEXED(app, shape, u32);
///
/// The storage bounds the values: with u32 every value must be below 32, which is asserted per flag.
#define CC_FLAG_ENUM_INDEXED(Namespace, EnumType, FlagStorageType) \
    CC_IMPL_FLAG_ENUM(Namespace, EnumType, FlagStorageType, ::cc::flag_encoding::bit_index)

/// Declare an enum whose values ALREADY ARE bit patterns a flag enum, so one value may name several bits.
///
///     namespace app { enum class usage : u32 { vertex = 1u << 0, index = 1u << 1, both = 0b11 }; }
///     CC_FLAG_ENUM_BITMASK(app, usage, u32);
///
/// Both macros exist, and neither is the default, because the choice is not detectable from the enum:
/// nothing about `e = 4` says whether it means bit 4 or bit 2, and picking the wrong one silently shifts every flag.
///
/// Write either at GLOBAL scope right after the enum, naming its namespace and the enum separately.
/// The namespace is an argument because the two halves have opposite scope requirements: a cc::custom::enum_traits
/// specialization is only legal at a namespace enclosing cc, while an operator on an enum is reachable only through ADL
/// and so must sit inside the enum's own namespace.
/// The macro reopens it for the operators itself, which is what keeps the enum's header from having to stay open around them.
///
/// EnumType must live in a namespace, and the storage is always spelled out — the enum's own underlying type is not assumed.
#define CC_FLAG_ENUM_BITMASK(Namespace, EnumType, FlagStorageType) \
    CC_IMPL_FLAG_ENUM(Namespace, EnumType, FlagStorageType, ::cc::flag_encoding::bit_mask)
