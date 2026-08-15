#pragma once

#include <clean-core/container/small_vector.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string_view.hh>
#include <versioned-document/fwd.hh>

#include <type_traits>

/// The canonical binary value codec.
///
/// A value is a self-describing byte sequence: a tag byte plus a payload.
/// Equality and hashing are defined on those bytes and on nothing else, and there is no structural comparison anywhere in this library.
///
/// That is only meaningful because each value has exactly one valid encoding, so the format is canonical and
/// [try_decode] enforces it — a non-canonical encoding is a decode error, never something repaired.
/// Tolerating one would silently break diffing, content addressing and every merge decision built on them.
///
/// The design is [the concept](../../docs/concept.md#values).
///
/// NOT FROZEN.
/// If a general-purpose any-value format ever lands elsewhere, this codec may be replaced by it.
/// That would be a BREAKING CHANGE to the .vdoc format rather than a refactor, and a migration may or may not be provided.
/// The file's `user_version` keeps that door open in principle; it is not a compatibility promise.
/// See [the settled choices](../../docs/decisions.md#the-codec-starts-in-vdoc-not-in-clean-core).

/// What a value holds — the tag byte that opens every encoded value.
///
/// These byte values are a FORMAT CONSTANT, pinned here.
/// Reordering the enum must never be able to change the format, so every enumerator carries its number explicitly.
enum class vdoc::value_kind : cc::u8
{
    null = 0,
    boolean = 1,
    integer = 2,
    number = 3,
    string = 4,
    bytes = 5,
    array = 6,
    object = 7,
};

/// Why bytes would not decode into a value.
/// String-free by design, so a caller localizes it and a test asserts on it exactly.
enum class vdoc::value_decode_error_kind : cc::u8
{
    /// The buffer ended in the middle of a tag, a length prefix or a payload.
    truncated,
    /// The tag byte is not one of the eight kinds.
    unknown_tag,
    /// A boolean payload that is neither 0 nor 1.
    invalid_boolean,
    /// A container's declared payload length does not fit its buffer, or a child overruns it.
    length_mismatch,
    /// A container's declared element count does not end exactly where its declared payload does.
    count_mismatch,
    /// The value decoded, but bytes remained after it.
    trailing_bytes,
    /// Object keys are not in ascending byte order.
    unsorted_keys,
    /// Two object entries carry the same key.
    duplicate_key,
    /// Nesting deeper than value_view::max_depth.
    depth_exceeded,
};

/// What was wrong and where.
/// `offset` is the byte offset into the buffer handed to try_decode, at the field that failed rather than at the value containing it.
struct vdoc::value_decode_error
{
    value_decode_error_kind kind = value_decode_error_kind::truncated;
    isize offset = 0;

    [[nodiscard]] friend constexpr bool operator==(value_decode_error const&, value_decode_error const&) = default;
};

namespace vdoc::impl
{
/// The whole encoding of the null value: one tag byte.
/// A default-constructed value_view points at this, so it carries no storage of its own.
inline constexpr byte null_encoding = byte(u8(value_kind::null));

/// The two byte-level operations every equality and hash in this library bottoms out in.
/// They live here so value and value_view share one definition, and so neither header pulls in the hashing machinery.
[[nodiscard]] bool bytes_equal(cc::span<byte const> a, cc::span<byte const> b);
[[nodiscard]] u64 hash_bytes(cc::span<byte const> b);

/// Orders two object keys the way the format defines: ascending by BYTE value, negative / zero / positive.
/// This is the one definition of that order — the builder sorts by it and the decoder validates against it, so they cannot drift.
///
/// Not cc::string_view::compare, which widens char to a SIGNED int.
/// Under that order a key byte >= 0x80 sorts before an ASCII one, so any object with a non-ASCII key would be built
/// in an order its own decoder rejects.
[[nodiscard]] int compare_key_bytes(cc::string_view a, cc::string_view b);
} // namespace vdoc::impl

/// A non-owning view of one encoded value.
///
/// Every accessor assumes the bytes already passed try_decode, which is the only route from untrusted bytes to a view.
/// Nothing here re-validates, so handing it unchecked bytes is a programmer error rather than an input error.
struct vdoc::value_view
{
    /// The deepest nesting try_decode accepts, counting the value itself as level 1.
    /// A FORMAT CONSTANT, and the reason a corrupt or hostile input cannot drive the decoder into unbounded recursion.
    static constexpr isize max_depth = 64;

    /// The null value.
    /// A default view is null rather than empty, so every accessor below stays total.
    constexpr value_view() = default;

    /// Wraps bytes that have already passed try_decode.
    /// Passing anything else is undefined behaviour, not a detected error.
    [[nodiscard]] static value_view from_validated_bytes(cc::span<byte const> bytes) { return value_view(bytes); }

    [[nodiscard]] value_kind kind() const { return value_kind(u8(_bytes[0])); }

    /// The canonical encoding, tag byte included.
    /// This is what everything durable commits to, and what equality and hashing look at.
    [[nodiscard]] cc::span<byte const> bytes() const { return _bytes; }

    [[nodiscard]] bool is_null() const { return kind() == value_kind::null; }

    /// kind() must be boolean.
    [[nodiscard]] bool as_bool() const;
    /// kind() must be integer.
    [[nodiscard]] i64 as_i64() const;
    /// kind() must be number.
    [[nodiscard]] f64 as_f64() const;
    /// kind() must be string; the result is not null-terminated and borrows this view's bytes.
    [[nodiscard]] cc::string_view as_string() const;
    /// kind() must be bytes.
    [[nodiscard]] cc::span<byte const> as_bytes() const;

    /// Array elements or object entries; 0 for every scalar kind.
    [[nodiscard]] isize size() const;

    /// An array element, or an object entry's value.
    /// kind() must be array or object, and `i` must be in [0, size()).
    ///
    /// This WALKS: it is O(i), not O(1), because entries are variable-length.
    /// What the length prefixes buy is that each step of that walk is O(1) — skipping a subtree never descends into it.
    [[nodiscard]] value_view element_at(isize i) const;

    /// An object entry's key.
    /// kind() must be object, and `i` must be in [0, size()).
    [[nodiscard]] cc::string_view key_at(isize i) const;

    /// Looks up an object entry by key; empty if absent, and empty for every non-object kind.
    ///
    /// A LINEAR scan that stops early once a key sorts past the target.
    /// Sorted keys make it exit early; they do not make it a binary search, because there is no offset table to jump through.
    [[nodiscard]] cc::optional<value_view> try_find(cc::string_view key) const;

    /// Byte equality, which is the only equality this library has.
    [[nodiscard]] friend bool operator==(value_view a, value_view b) { return impl::bytes_equal(a._bytes, b._bytes); }
    [[nodiscard]] friend bool operator!=(value_view a, value_view b) { return !(a == b); }

    /// ADL customization point (see clean-core/common/hash.hh), over the bytes and nothing else.
    [[nodiscard]] friend u64 hash(value_view v) { return impl::hash_bytes(v._bytes); }

private:
    explicit constexpr value_view(cc::span<byte const> bytes) : _bytes(bytes) {}

    cc::span<byte const> _bytes = cc::span<byte const>(&impl::null_encoding, 1);
};

/// An owning value.
///
/// Backed by a cc::small_vector, whose inline buffer auto-grows to fill its 48-byte footprint — so N = 1 already yields
/// 36 inline bytes, which covers essentially every real value without an allocation.
/// A larger N buys nothing until it grows the struct, and growing the struct costs every property in a document.
///
/// Bulk data does not belong here: a mesh or a texture is a blob, referenced by an asset id string.
struct vdoc::value
{
    /// The null value.
    value() { _storage.push_back(byte(u8(value_kind::null))); }

    [[nodiscard]] static value of_null();
    [[nodiscard]] static value of(bool v);
    [[nodiscard]] static value of(f32 v);
    [[nodiscard]] static value of(f64 v);
    [[nodiscard]] static value of(cc::string_view v);

    /// Without this overload value::of("wall") would pick of(bool) through the pointer-to-bool conversion.
    [[nodiscard]] static value of(char const* v) { return of(cc::string_view(v)); }

    /// Any integer type other than bool, widened to the one 8-byte integer the format has.
    template <class T>
        requires(std::is_integral_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    [[nodiscard]] static value of(T v)
    {
        return of_i64(i64(v));
    }

    [[nodiscard]] static value of_i64(i64 v);
    [[nodiscard]] static value of_bytes(cc::span<byte const> v);

    /// Takes ownership of bytes that have already passed try_decode.
    [[nodiscard]] static value from_validated_bytes(cc::span<byte const> bytes);

    [[nodiscard]] value_view view() const { return value_view::from_validated_bytes(bytes()); }
    [[nodiscard]] operator value_view() const { return view(); }

    [[nodiscard]] cc::span<byte const> bytes() const { return cc::span<byte const>(_storage.data(), _storage.size()); }

    [[nodiscard]] value_kind kind() const { return view().kind(); }
    [[nodiscard]] bool is_null() const { return view().is_null(); }
    [[nodiscard]] bool as_bool() const { return view().as_bool(); }
    [[nodiscard]] i64 as_i64() const { return view().as_i64(); }
    [[nodiscard]] f64 as_f64() const { return view().as_f64(); }
    [[nodiscard]] cc::string_view as_string() const { return view().as_string(); }
    [[nodiscard]] cc::span<byte const> as_bytes() const { return view().as_bytes(); }
    [[nodiscard]] isize size() const { return view().size(); }
    [[nodiscard]] value_view element_at(isize i) const { return view().element_at(i); }
    [[nodiscard]] cc::string_view key_at(isize i) const { return view().key_at(i); }
    [[nodiscard]] cc::optional<value_view> try_find(cc::string_view key) const { return view().try_find(key); }

    /// True while the encoding fits the inline buffer, which is what "a small value does not allocate" means.
    [[nodiscard]] bool is_inline() const { return _storage.is_inline(); }

    [[nodiscard]] friend bool operator==(value const& a, value const& b)
    {
        return impl::bytes_equal(a.bytes(), b.bytes());
    }
    [[nodiscard]] friend bool operator!=(value const& a, value const& b) { return !(a == b); }
    [[nodiscard]] friend u64 hash(value const& v) { return impl::hash_bytes(v.bytes()); }

private:
    /// Storage sized to `size` with undefined contents, for a factory that then writes every byte of it.
    [[nodiscard]] static value make_uninitialized(isize size);
    [[nodiscard]] cc::span<byte> mutable_bytes() { return cc::span<byte>(_storage.data(), _storage.size()); }

    cc::small_vector<byte, 1> _storage;
};

namespace vdoc
{
/// The only route from bytes to a value_view, and the one place canonicality is enforced.
///
/// Validates in a single pass and REPAIRS NOTHING: unsorted or duplicate object keys, a boolean payload other than 0 or 1,
/// a length prefix that disagrees with its contents, trailing bytes and excessive nesting are all errors.
///
/// The returned view borrows `bytes`, which must outlive it.
[[nodiscard]] cc::result<value_view, value_decode_error> try_decode(cc::span<byte const> bytes);

/// Advances past the first value in `bytes` and returns what follows, for walking a buffer of adjacent values.
/// `bytes` must begin with a value that has already passed try_decode.
[[nodiscard]] cc::span<byte const> skip_value(cc::span<byte const> bytes);

/// The encoded size of the first value in `bytes`, which is what skip_value advances by.
/// `bytes` must begin with a value that has already passed try_decode.
[[nodiscard]] isize encoded_size(cc::span<byte const> bytes);
} // namespace vdoc
