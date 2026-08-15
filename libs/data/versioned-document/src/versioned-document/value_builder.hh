#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <versioned-document/value.hh>

/// Builds the two container kinds, arrays and objects.
///
/// This is the ONLY place object keys are sorted.
/// A decoder validates that order and never repairs it, so a value that reaches storage was sorted here or it does not decode.

/// Why a builder's contents would not make a value.
/// Neither is reachable from an encoded input, which is why these are separate from value_decode_error.
enum class vdoc::value_build_error : cc::u8
{
    /// Two entries of one object carry the same key.
    duplicate_key,
    /// A key, or a container's whole payload, is longer than the u32 prefix that has to describe it.
    too_large,
};

/// Builds one array or one object, then hands out the finished encoding.
///
/// Nesting is by COMPOSITION rather than by an in-place nesting API:
///
///     auto const v = vdoc::value_builder::object()
///                        .set("name", "wall")
///                        .set("p", vdoc::value_builder::array().push(1.0).push(2.0).build())
///                        .build();
///
/// Each level is therefore canonicalized where it is built, and a finished sub-value is just another value to push.
///
/// Building is non-destructive: a builder can be built twice, and reused and extended in between.
class vdoc::value_builder
{
public:
    [[nodiscard]] static value_builder array() { return value_builder(value_kind::array); }
    [[nodiscard]] static value_builder object() { return value_builder(value_kind::object); }

    /// The kind this builder produces.
    [[nodiscard]] value_kind kind() const { return _kind; }

    /// Entries pushed or set so far.
    /// For an object this counts duplicate keys too, which try_build then rejects.
    [[nodiscard]] isize size() const { return _entries.size(); }

    // ---- arrays ----

    /// Appends to an array, in insertion order — an array is not sorted.
    value_builder& push(value_view v) { return push_encoded(v.bytes()); }
    value_builder& push(value const& v) { return push_encoded(v.bytes()); }

    /// Appends anything value::of accepts: a bool, any integer, a float, a string.
    template <class T>
        requires requires(T&& v) { value::of(cc::forward<T>(v)); }
    value_builder& push(T&& v)
    {
        return push_encoded(value::of(cc::forward<T>(v)).bytes());
    }

    value_builder& push_null() { return push_encoded(value::of_null().bytes()); }
    value_builder& push_bytes(cc::span<byte const> v) { return push_encoded(value::of_bytes(v).bytes()); }

    // ---- objects ----

    /// Adds an object entry.
    /// Order of calls does not matter — try_build sorts by key — but adding the same key twice is a build error rather than an overwrite.
    value_builder& set(cc::string_view key, value_view v) { return set_encoded(key, v.bytes()); }
    value_builder& set(cc::string_view key, value const& v) { return set_encoded(key, v.bytes()); }

    /// Adds an object entry holding anything value::of accepts.
    template <class T>
        requires requires(T&& v) { value::of(cc::forward<T>(v)); }
    value_builder& set(cc::string_view key, T&& v)
    {
        return set_encoded(key, value::of(cc::forward<T>(v)).bytes());
    }

    value_builder& set_null(cc::string_view key) { return set_encoded(key, value::of_null().bytes()); }
    value_builder& set_bytes(cc::string_view key, cc::span<byte const> v)
    {
        return set_encoded(key, value::of_bytes(v).bytes());
    }

    // ---- finishing ----

    /// The finished value, sorting object keys on the way out.
    /// Fails only on a duplicate key or on something too large for a u32 prefix.
    [[nodiscard]] cc::result<value, value_build_error> try_build() const;

    /// try_build for callers whose contents cannot be wrong — a duplicate key or an oversized payload asserts.
    /// Reach for try_build when the entries came from outside the program.
    [[nodiscard]] value build() const;

private:
    struct entry
    {
        cc::string key; // empty and unused for an array
        cc::vector<byte> encoded;
    };

    explicit value_builder(value_kind kind) : _kind(kind) {}

    value_builder& push_encoded(cc::span<byte const> encoded);
    value_builder& set_encoded(cc::string_view key, cc::span<byte const> encoded);

    value_kind _kind;
    cc::vector<entry> _entries;
};
