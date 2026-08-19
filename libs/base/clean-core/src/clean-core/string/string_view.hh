#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/hash.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/pair.hh>
#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh>
#include <clean-core/string/char_predicates.hh>

/// Non-owning view over a contiguous sequence of char, interpreted as UTF-8.
/// A char const* plus an isize, trivially copyable.
/// It owns nothing, so the viewed storage must outlive it.
///
/// NOT null-terminated: nothing guarantees a '\0' after the last byte, so data() must never reach a C API
/// that expects one.
/// cc::string::c_str_materialize() is how a terminated pointer is obtained.
///
/// libs/base/clean-core/docs/strings.md owns the lifetime, hashing and conversion contracts.
struct cc::string_view
{
    // construction
public:
    /// Default string_view is empty: data() == nullptr, size() == 0.
    constexpr string_view() = default;

    /// Prevent construction from nullptr to avoid undefined behavior (compile-time error instead of runtime crash).
    string_view(nullptr_t) = delete;

    // keep triviality
    constexpr string_view(string_view const&) = default;
    constexpr string_view(string_view&&) = default;
    constexpr string_view& operator=(string_view const&) = default;
    constexpr string_view& operator=(string_view&&) = default;
    constexpr ~string_view() = default;

    /// Creates a string_view viewing [ptr, ptr+size).
    /// Precondition: size >= 0, and ptr must not be null unless size == 0.
    constexpr explicit string_view(char const* ptr, isize size) : _data(ptr), _size(size)
    {
        CC_ASSERT(size >= 0, "string_view size must be non-negative");
        CC_ASSERT(ptr != nullptr || size == 0, "null pointer only allowed for empty range");
    }

    /// Creates a string_view viewing [begin, end).
    /// Precondition: begin <= end, and begin must not be null unless begin == end.
    constexpr explicit string_view(char const* begin, char const* end) : _data(begin), _size(end - begin)
    {
        CC_ASSERT(begin <= end, "invalid pointer range");
        CC_ASSERT(begin != nullptr || begin == end, "null pointer only allowed for empty range");
    }

    /// Creates a string_view from a null-terminated C string, whose length is found by scanning for '\0'.
    /// The view excludes the terminator.
    /// Precondition: cstr must not be null.
    ///
    /// A char array — a literal, or a buffer — decays to a pointer and arrives here, so the length is the string's and never the array's.
    /// `char buf[100] = "hello"` therefore views 5 chars, and an UNTERMINATED buffer is the one thing this cannot read: give it a size, string_view(buf, n).
    constexpr string_view(char const* cstr)
    {
        CC_ASSERT(cstr != nullptr, "string_view cannot be constructed from nullptr");
        _data = cstr;
        _size = compute_length(cstr);
    }

    /// Creates a string_view from any container whose .data() converts to char const* and which has .size().
    /// The container must outlive the view.
    /// Passing a temporary is safe only where the view is used immediately, such as a function argument.
    template <class Container>
        requires requires(Container&& c) {
            { c.data() } -> std::convertible_to<char const*>;
            { c.size() } -> std::convertible_to<isize>;
        }
    constexpr string_view(Container&& c) : _data(c.data()), _size(static_cast<isize>(c.size()))
    {
    }

    // element access
public:
    /// Returns a reference to the character at index i.
    /// Precondition: 0 <= i < size().
    [[nodiscard]] constexpr char operator[](isize i) const
    {
        CC_ASSERT(0 <= i && i < _size, "index out of bounds");
        return _data[i];
    }

    /// Returns a reference to the first character.
    /// Precondition: !empty().
    [[nodiscard]] constexpr char front() const
    {
        CC_ASSERT(_size > 0, "front() called on empty string_view");
        return _data[0];
    }

    /// Returns a reference to the last character.
    /// Precondition: !empty().
    [[nodiscard]] constexpr char back() const
    {
        CC_ASSERT(_size > 0, "back() called on empty string_view");
        return _data[_size - 1];
    }

    /// Returns a pointer to the underlying contiguous storage, which may be nullptr for an empty view.
    ///
    /// NOT guaranteed to be null-terminated, so it must not be passed to a C API expecting '\0'.
    [[nodiscard]] constexpr char const* data() const { return _data; }

    // iterators
public:
    /// Returns a pointer to the first character; nullptr if empty.
    /// Enables range-based for loops.
    [[nodiscard]] constexpr char const* begin() const { return _data; }
    /// Returns a pointer to one past the last character.
    [[nodiscard]] constexpr char const* end() const { return _data + _size; }

    // queries
public:
    /// Returns the number of bytes in the string_view, rather than the number of UTF-8 code points.
    [[nodiscard]] constexpr isize size() const { return _size; }
    /// Returns true if size() == 0.
    [[nodiscard]] constexpr bool empty() const { return _size == 0; }

    // span access
public:
    /// The viewed chars as a span (no trailing terminator; string_view has none).
    [[nodiscard]] constexpr cc::span<char const> as_span() const { return cc::span<char const>(_data, _size); }
    /// The viewed chars as immutable raw bytes.
    [[nodiscard]] cc::span<cc::byte const> as_bytes() const { return as_span().as_bytes(); }

    // substring operations
public:
    /// Returns a subview starting at offset to the end of the string.
    /// Precondition: offset <= size().
    [[nodiscard]] constexpr string_view subview(isize offset) const
    {
        CC_ASSERT(offset <= _size, "subview offset out of range");
        return string_view(_data + offset, _size - offset);
    }

    /// Returns the subview [r.offset, r.offset + r.size).
    /// Precondition: r.size >= 0 && r.offset >= 0 && r.offset + r.size <= size().
    [[nodiscard]] constexpr string_view subview(offset_size r) const
    {
        CC_ASSERT(r.size >= 0, "subview size must be non-negative");
        CC_ASSERT(r.offset >= 0 && r.offset + r.size <= _size, "subview range out of bounds");
        return string_view(_data + r.offset, r.size);
    }

    /// Returns the subview [r.start, r.end).
    /// Precondition: r.end >= r.start && r.start >= 0 && r.end <= size().
    [[nodiscard]] constexpr string_view subview(start_end r) const
    {
        CC_ASSERT(r.end >= r.start, "subview end must not precede start");
        CC_ASSERT(r.start >= 0 && r.end <= _size, "subview range out of bounds");
        return string_view(_data + r.start, r.end - r.start);
    }

    /// Returns a subview at offset with the given size, clamped to valid bounds instead of asserting.
    /// An offset past size() gives an empty view, and an over-long size is truncated to fit.
    [[nodiscard]] constexpr string_view subview_clamped(isize offset, isize size) const
    {
        return string_view(_data + offset, offset > _size ? 0 : offset + size > _size ? _size - offset : size);
    }

    /// Removes the first n characters from the view.
    /// Precondition: 0 <= n <= size().
    constexpr void remove_prefix(isize n)
    {
        CC_ASSERT(0 <= n && n <= _size, "remove_prefix count out of range");
        _data += n;
        _size -= n;
    }

    /// Removes the last n characters from the view.
    /// Precondition: 0 <= n <= size().
    constexpr void remove_suffix(isize n)
    {
        CC_ASSERT(0 <= n && n <= _size, "remove_suffix count out of range");
        _size -= n;
    }

    // prefix/suffix matching operations
    //
    // Every member of this family takes an EqualF comparing two chars, defaulting to cc::equal_case_sensitive
    // from char_predicates.hh — pass cc::equal_case_insensitive to match without regard to case.
    // starts_with / ends_with / contains are not part of the family and are always case-sensitive.
    // The _of statics take both views explicitly; the _with members compare against this view and return
    // views into this view's data.
    // A strip_ member mutates in place, while a stripped_ member returns a copy.
public:
    // Forward declarations of result types (defined at bottom of file)
    struct decomposed_prefix;
    struct decomposed_suffix;
    struct decomposed_affixes;

    /// Decomposes two views at their common prefix, comparing forwards from the start of each.
    /// Returns the matching prefix of each view, plus what remains of each after it.
    ///
    /// Usage:
    ///   auto decomp = string_view::decompose_matching_prefix("hello world", "hello there");
    ///   // decomp.prefix_lhs == "hello "
    ///   // decomp.middle_lhs == "world"
    ///   // decomp.middle_rhs == "there"
    template <class EqualF = equal_case_sensitive>
    [[nodiscard]] static constexpr decomposed_prefix decompose_matching_prefix(string_view lhs,
                                                                               string_view rhs,
                                                                               EqualF&& eq = {});

    /// Decomposes two views at their common suffix, comparing backwards from the end of each.
    /// Returns what remains of each view before the suffix, plus the matching suffix of each.
    ///
    /// Usage:
    ///   auto decomp = string_view::decompose_matching_suffix("prefix_test", "other_test");
    ///   // decomp.suffix_lhs == "_test"
    ///   // decomp.middle_lhs == "prefix"
    ///   // decomp.middle_rhs == "other"
    template <class EqualF = equal_case_sensitive>
    [[nodiscard]] static constexpr decomposed_suffix decompose_matching_suffix(string_view lhs,
                                                                               string_view rhs,
                                                                               EqualF&& eq = {});

    /// Decomposes two views at both their common prefix and their common suffix.
    /// The prefix is found first, and the suffix is then found in what remains.
    ///
    /// Usage:
    ///   auto decomp = string_view::decompose_matching_affixes("prefix_A_suffix", "prefix_B_suffix");
    ///   // decomp.prefix_lhs == "prefix_"
    ///   // decomp.middle_lhs == "A"
    ///   // decomp.middle_rhs == "B"
    ///   // decomp.suffix_lhs == "_suffix"
    template <class EqualF = equal_case_sensitive>
    [[nodiscard]] static constexpr decomposed_affixes decompose_matching_affixes(string_view lhs,
                                                                                 string_view rhs,
                                                                                 EqualF&& eq = {});

    /// Returns the matching prefix from lhs
    ///
    /// Usage:
    ///   auto prefix = string_view::matching_prefix_of("hello world", "hello there");
    ///   // prefix == "hello "
    template <class EqualF = equal_case_sensitive>
    [[nodiscard]] static constexpr string_view matching_prefix_of(string_view lhs, string_view rhs, EqualF&& eq = {});

    /// Returns the matching suffix from lhs
    ///
    /// Usage:
    ///   auto suffix = string_view::matching_suffix_of("prefix_test", "other_test");
    ///   // suffix == "_test"
    template <class EqualF = equal_case_sensitive>
    [[nodiscard]] static constexpr string_view matching_suffix_of(string_view lhs, string_view rhs, EqualF&& eq = {});

    /// Returns the matching prefix and suffix from lhs
    ///
    /// Usage:
    ///   auto [prefix, suffix] = string_view::matching_affixes_of("pre_A_suf", "pre_B_suf");
    ///   // prefix == "pre_"
    ///   // suffix == "_suf"
    template <class EqualF = equal_case_sensitive>
    [[nodiscard]] static constexpr cc::pair<string_view, string_view> matching_affixes_of(string_view lhs,
                                                                                          string_view rhs,
                                                                                          EqualF&& eq = {});

    /// Returns lhs with its matching prefix removed
    ///
    /// Usage:
    ///   auto stripped = string_view::strip_matching_prefix_of("hello world", "hello there");
    ///   // stripped == "world"
    template <class EqualF = equal_case_sensitive>
    [[nodiscard]] static constexpr string_view strip_matching_prefix_of(string_view lhs,
                                                                        string_view rhs,
                                                                        EqualF&& eq = {});

    /// Returns lhs with its matching suffix removed
    ///
    /// Usage:
    ///   auto stripped = string_view::strip_matching_suffix_of("prefix_test", "other_test");
    ///   // stripped == "prefix"
    template <class EqualF = equal_case_sensitive>
    [[nodiscard]] static constexpr string_view strip_matching_suffix_of(string_view lhs,
                                                                        string_view rhs,
                                                                        EqualF&& eq = {});

    /// Returns lhs with its matching prefix and suffix removed
    ///
    /// Usage:
    ///   auto stripped = string_view::strip_matching_affixes_of("pre_A_suf", "pre_B_suf");
    ///   // stripped == "A"
    template <class EqualF = equal_case_sensitive>
    [[nodiscard]] static constexpr string_view strip_matching_affixes_of(string_view lhs,
                                                                         string_view rhs,
                                                                         EqualF&& eq = {});

    /// Returns the matching prefix between this view and another
    /// The returned view points to this view's data
    template <class EqualF = equal_case_sensitive>
    [[nodiscard]] constexpr string_view matching_prefix_with(string_view other, EqualF&& eq = {}) const;

    /// Returns the matching suffix between this view and another
    /// The returned view points to this view's data
    template <class EqualF = equal_case_sensitive>
    [[nodiscard]] constexpr string_view matching_suffix_with(string_view other, EqualF&& eq = {}) const;

    /// Returns the matching prefix and suffix between this view and another
    /// Both returned views point to this view's data
    template <class EqualF = equal_case_sensitive>
    [[nodiscard]] constexpr cc::pair<string_view, string_view> matching_affixes_with(string_view other,
                                                                                     EqualF&& eq = {}) const;

    /// Removes the matching prefix between this view and another from this view (modifies in place)
    template <class EqualF = equal_case_sensitive>
    constexpr void strip_matching_prefix_with(string_view other, EqualF&& eq = {});

    /// Removes the matching suffix between this view and another from this view (modifies in place)
    template <class EqualF = equal_case_sensitive>
    constexpr void strip_matching_suffix_with(string_view other, EqualF&& eq = {});

    /// Removes the matching prefix and suffix between this view and another from this view (modifies in place)
    template <class EqualF = equal_case_sensitive>
    constexpr void strip_matching_affixes_with(string_view other, EqualF&& eq = {});

    /// Returns a copy of this view with the matching prefix removed (does not modify this view)
    template <class EqualF = equal_case_sensitive>
    [[nodiscard]] constexpr string_view stripped_matching_prefix_with(string_view other, EqualF&& eq = {}) const;

    /// Returns a copy of this view with the matching suffix removed (does not modify this view)
    template <class EqualF = equal_case_sensitive>
    [[nodiscard]] constexpr string_view stripped_matching_suffix_with(string_view other, EqualF&& eq = {}) const;

    /// Returns a copy of this view with the matching prefix and suffix removed (does not modify this view)
    template <class EqualF = equal_case_sensitive>
    [[nodiscard]] constexpr string_view stripped_matching_affixes_with(string_view other, EqualF&& eq = {}) const;

    // comparison
public:
    /// Lexicographically compares this string_view with another.
    /// Returns: <0 if *this < other, 0 if equal, >0 if *this > other.
    ///
    /// Compares by UNSIGNED byte value, so the order is the byte order the content actually has.
    /// `char` is signed on our platforms, so comparing it directly would sort any byte >= 0x80 before every ASCII one —
    /// which would put UTF-8 text in an order no other implementation agrees with, this one included once the bytes are
    /// written to a file or hashed.
    [[nodiscard]] constexpr int compare(string_view other) const
    {
        auto const min_size = _size < other._size ? _size : other._size;
        for (isize i = 0; i < min_size; ++i)
        {
            if (_data[i] != other._data[i])
                return int(u8(_data[i])) - int(u8(other._data[i]));
        }
        return _size < other._size ? -1 : (_size > other._size ? 1 : 0);
    }

    /// Returns true if this string_view starts with the given prefix.
    [[nodiscard]] constexpr bool starts_with(string_view prefix) const
    {
        if (prefix._size > _size)
            return false;
        for (isize i = 0; i < prefix._size; ++i)
        {
            if (_data[i] != prefix._data[i])
                return false;
        }
        return true;
    }

    /// Returns true if this string_view starts with the given character.
    [[nodiscard]] constexpr bool starts_with(char c) const { return _size > 0 && _data[0] == c; }

    /// Returns true if this string_view ends with the given suffix.
    [[nodiscard]] constexpr bool ends_with(string_view suffix) const
    {
        if (suffix._size > _size)
            return false;
        for (isize i = 0; i < suffix._size; ++i)
        {
            if (_data[_size - suffix._size + i] != suffix._data[i])
                return false;
        }
        return true;
    }

    /// Returns true if this string_view ends with the given character.
    [[nodiscard]] constexpr bool ends_with(char c) const { return _size > 0 && _data[_size - 1] == c; }

    /// Returns true if this string_view contains the given substring.
    [[nodiscard]] constexpr bool contains(string_view substring) const { return find(substring) != -1; }

    /// Returns true if this string_view contains the given character.
    [[nodiscard]] constexpr bool contains(char c) const { return find(c) != -1; }

    // search operations
public:
    /// Finds the first occurrence of substring, starting at position pos.
    /// Returns the index of the first character, or -1 if not found.
    /// Precondition: 0 <= pos <= size().
    [[nodiscard]] constexpr isize find(string_view substring, isize pos = 0) const
    {
        CC_ASSERT(0 <= pos && pos <= _size, "find position out of range");
        if (substring._size == 0)
            return pos;
        if (substring._size > _size - pos)
            return -1;

        for (isize i = pos; i <= _size - substring._size; ++i)
        {
            bool match = true;
            for (isize j = 0; j < substring._size; ++j)
            {
                if (_data[i + j] != substring._data[j])
                {
                    match = false;
                    break;
                }
            }
            if (match)
                return i;
        }
        return -1;
    }

    /// Finds the first occurrence of character c, starting at position pos.
    /// Returns the index, or -1 if not found.
    /// Precondition: 0 <= pos <= size().
    [[nodiscard]] constexpr isize find(char c, isize pos = 0) const
    {
        CC_ASSERT(0 <= pos && pos <= _size, "find position out of range");
        for (isize i = pos; i < _size; ++i)
        {
            if (_data[i] == c)
                return i;
        }
        return -1;
    }

    /// Finds the last occurrence of substring, searching backwards from position pos.
    /// If pos is -1, searches from the end.
    /// Returns the index of the first character of the found substring, or -1 if not found.
    [[nodiscard]] constexpr isize rfind(string_view substring, isize pos = -1) const
    {
        if (substring._size == 0)
            return pos == -1 ? _size : (pos < _size ? pos : _size);
        if (substring._size > _size)
            return -1;

        auto const start_pos = (pos == -1 || pos > _size - substring._size) ? (_size - substring._size) : pos;
        for (isize i = start_pos; i >= 0; --i)
        {
            bool match = true;
            for (isize j = 0; j < substring._size; ++j)
            {
                if (_data[i + j] != substring._data[j])
                {
                    match = false;
                    break;
                }
            }
            if (match)
                return i;
        }
        return -1;
    }

    /// Finds the last occurrence of character c, searching backwards from position pos.
    /// If pos is -1, searches from the end.
    /// Returns the index, or -1 if not found.
    [[nodiscard]] constexpr isize rfind(char c, isize pos = -1) const
    {
        auto const start_pos = (pos == -1 || pos >= _size) ? (_size - 1) : pos;
        for (isize i = start_pos; i >= 0; --i)
        {
            if (_data[i] == c)
                return i;
        }
        return -1;
    }

    // comparison operators (hidden friends)
public:
    /// Equality comparison between two string_views.
    [[nodiscard]] friend constexpr bool operator==(string_view lhs, string_view rhs)
    {
        if (lhs._size != rhs._size)
            return false;
        for (isize i = 0; i < lhs._size; ++i)
        {
            if (lhs._data[i] != rhs._data[i])
                return false;
        }
        return true;
    }

    /// Inequality comparison between two string_views.
    [[nodiscard]] friend constexpr bool operator!=(string_view lhs, string_view rhs) { return !(lhs == rhs); }

    /// Less-than comparison between two string_views.
    [[nodiscard]] friend constexpr bool operator<(string_view lhs, string_view rhs) { return lhs.compare(rhs) < 0; }

    /// Greater-than comparison between two string_views.
    [[nodiscard]] friend constexpr bool operator>(string_view lhs, string_view rhs) { return lhs.compare(rhs) > 0; }

    /// Less-than-or-equal comparison between two string_views.
    [[nodiscard]] friend constexpr bool operator<=(string_view lhs, string_view rhs) { return lhs.compare(rhs) <= 0; }

    /// Greater-than-or-equal comparison between two string_views.
    [[nodiscard]] friend constexpr bool operator>=(string_view lhs, string_view rhs) { return lhs.compare(rhs) >= 0; }

    // hashing
public:
    /// Structural hash over the viewed bytes via cc::make_hash_of_bytes (XXH3-64).
    /// Equal content hashes equally to cc::string, so a string_view can be used for heterogeneous lookup in
    /// a string-keyed map.
    /// libs/base/clean-core/docs/benchmarks/string-hash-benchmark.md is why XXH3 is the chosen default.
    [[nodiscard]] friend u64 hash(string_view v) { return cc::make_hash_of_bytes(v.as_bytes()); }

    // members
private:
    /// Helper to compute length of null-terminated string at compile time.
    static constexpr isize compute_length(char const* cstr)
    {
        isize len = 0;
        while (cstr[len] != '\0')
            ++len;
        return len;
    }

    char const* _data = nullptr;
    isize _size = 0;
};

// string_view is a borrow range: its validity is independent of the view object.
namespace cc
{
template <>
inline constexpr bool enable_borrowed_range<string_view> = true;
} // namespace cc

namespace cc::custom
{
/// A char array hashes as the string it holds — exactly the bytes string_view(arr) views, so the length is the string's and not the array bound.
/// Without this a bare literal has no hash at all, because `m["axis"]` deduces the probe as char[N] and never as string_view.
/// Equality already reads a char array through the same conversion, so the hash has to agree or heterogeneous lookup would silently miss.
/// An unterminated buffer is the one case that must not go through here: view it explicitly as string_view(buf, size).
template <isize N>
struct hash_trait<char[N]>
{
    [[nodiscard]] static u64 hash(char const (&arr)[N]) { return cc::make_hash(cc::string_view(arr)); }
};
} // namespace cc::custom

// ============================================================================
// Implementation of prefix/suffix matching operations
// ============================================================================

/// Result type for decompose_matching_prefix
/// Contains the common prefix from both views and the remaining middle parts
struct cc::string_view::decomposed_prefix
{
    string_view prefix_lhs; ///< Matching prefix from lhs view
    string_view prefix_rhs; ///< Matching prefix from rhs view (same content, different pointer)
    string_view middle_lhs; ///< Remaining part of lhs after prefix
    string_view middle_rhs; ///< Remaining part of rhs after prefix
};

/// Result type for decompose_matching_suffix
/// Contains the remaining middle parts and the common suffix from both views
struct cc::string_view::decomposed_suffix
{
    string_view middle_lhs; ///< Remaining part of lhs before suffix
    string_view middle_rhs; ///< Remaining part of rhs before suffix
    string_view suffix_lhs; ///< Matching suffix from lhs view
    string_view suffix_rhs; ///< Matching suffix from rhs view (same content, different pointer)
};

/// Result type for decompose_matching_affixes
/// Contains the common prefix, remaining middle parts, and common suffix from both views
struct cc::string_view::decomposed_affixes
{
    string_view prefix_lhs; ///< Matching prefix from lhs view
    string_view prefix_rhs; ///< Matching prefix from rhs view (same content, different pointer)
    string_view middle_lhs; ///< Remaining part of lhs between prefix and suffix
    string_view middle_rhs; ///< Remaining part of rhs between prefix and suffix
    string_view suffix_lhs; ///< Matching suffix from lhs view
    string_view suffix_rhs; ///< Matching suffix from rhs view (same content, different pointer)
};

// ============================================================================
// Core decomposition functions (see declarations above for full documentation)
// ============================================================================

template <class EqualF>
constexpr cc::string_view::decomposed_prefix cc::string_view::decompose_matching_prefix(string_view lhs,
                                                                                        string_view rhs,
                                                                                        EqualF&& eq)
{
    auto const min_size = lhs.size() < rhs.size() ? lhs.size() : rhs.size();
    isize prefix_len = 0;

    while (prefix_len < min_size && eq(lhs[prefix_len], rhs[prefix_len]))
        ++prefix_len;

    return {.prefix_lhs = string_view(lhs.data(), prefix_len),
            .prefix_rhs = string_view(rhs.data(), prefix_len),
            .middle_lhs = string_view(lhs.data() + prefix_len, lhs.size() - prefix_len),
            .middle_rhs = string_view(rhs.data() + prefix_len, rhs.size() - prefix_len)};
}

template <class EqualF>
constexpr cc::string_view::decomposed_suffix cc::string_view::decompose_matching_suffix(string_view lhs,
                                                                                        string_view rhs,
                                                                                        EqualF&& eq)
{
    auto const min_size = lhs.size() < rhs.size() ? lhs.size() : rhs.size();
    isize suffix_len = 0;

    while (suffix_len < min_size && eq(lhs[lhs.size() - 1 - suffix_len], rhs[rhs.size() - 1 - suffix_len]))
        ++suffix_len;

    return {.middle_lhs = string_view(lhs.data(), lhs.size() - suffix_len),
            .middle_rhs = string_view(rhs.data(), rhs.size() - suffix_len),
            .suffix_lhs = string_view(lhs.data() + lhs.size() - suffix_len, suffix_len),
            .suffix_rhs = string_view(rhs.data() + rhs.size() - suffix_len, suffix_len)};
}

template <class EqualF>
constexpr cc::string_view::decomposed_affixes cc::string_view::decompose_matching_affixes(string_view lhs,
                                                                                          string_view rhs,
                                                                                          EqualF&& eq)
{
    auto const min_size = lhs.size() < rhs.size() ? lhs.size() : rhs.size();

    // Find common prefix
    isize prefix_len = 0;
    while (prefix_len < min_size && eq(lhs[prefix_len], rhs[prefix_len]))
        ++prefix_len;

    // Find common suffix in the remaining parts
    auto const lhs_remaining_size = lhs.size() - prefix_len;
    auto const rhs_remaining_size = rhs.size() - prefix_len;
    auto const min_remaining_size = lhs_remaining_size < rhs_remaining_size ? lhs_remaining_size : rhs_remaining_size;

    isize suffix_len = 0;
    while (suffix_len < min_remaining_size && eq(lhs[lhs.size() - 1 - suffix_len], rhs[rhs.size() - 1 - suffix_len]))
        ++suffix_len;

    return {.prefix_lhs = string_view(lhs.data(), prefix_len),
            .prefix_rhs = string_view(rhs.data(), prefix_len),
            .middle_lhs = string_view(lhs.data() + prefix_len, lhs.size() - prefix_len - suffix_len),
            .middle_rhs = string_view(rhs.data() + prefix_len, rhs.size() - prefix_len - suffix_len),
            .suffix_lhs = string_view(lhs.data() + lhs.size() - suffix_len, suffix_len),
            .suffix_rhs = string_view(rhs.data() + rhs.size() - suffix_len, suffix_len)};
}

// ============================================================================
// Static wrapper functions returning matching parts
// ============================================================================

template <class EqualF>
constexpr cc::string_view cc::string_view::matching_prefix_of(string_view lhs, string_view rhs, EqualF&& eq)
{
    return string_view::decompose_matching_prefix(lhs, rhs, static_cast<EqualF&&>(eq)).prefix_lhs;
}

template <class EqualF>
constexpr cc::string_view cc::string_view::matching_suffix_of(string_view lhs, string_view rhs, EqualF&& eq)
{
    return string_view::decompose_matching_suffix(lhs, rhs, static_cast<EqualF&&>(eq)).suffix_lhs;
}

template <class EqualF>
constexpr cc::pair<cc::string_view, cc::string_view> cc::string_view::matching_affixes_of(string_view lhs,
                                                                                          string_view rhs,
                                                                                          EqualF&& eq)
{
    auto const decomp = string_view::decompose_matching_affixes(lhs, rhs, static_cast<EqualF&&>(eq));
    return {decomp.prefix_lhs, decomp.suffix_lhs};
}

// ============================================================================
// Static wrapper functions returning stripped parts
// ============================================================================

template <class EqualF>
constexpr cc::string_view cc::string_view::strip_matching_prefix_of(string_view lhs, string_view rhs, EqualF&& eq)
{
    return string_view::decompose_matching_prefix(lhs, rhs, static_cast<EqualF&&>(eq)).middle_lhs;
}

template <class EqualF>
constexpr cc::string_view cc::string_view::strip_matching_suffix_of(string_view lhs, string_view rhs, EqualF&& eq)
{
    return string_view::decompose_matching_suffix(lhs, rhs, static_cast<EqualF&&>(eq)).middle_lhs;
}

template <class EqualF>
constexpr cc::string_view cc::string_view::strip_matching_affixes_of(string_view lhs, string_view rhs, EqualF&& eq)
{
    return string_view::decompose_matching_affixes(lhs, rhs, static_cast<EqualF&&>(eq)).middle_lhs;
}

// ============================================================================
// Member functions returning matching parts
// ============================================================================

template <class EqualF>
constexpr cc::string_view cc::string_view::matching_prefix_with(string_view other, EqualF&& eq) const
{
    return string_view::decompose_matching_prefix(*this, other, static_cast<EqualF&&>(eq)).prefix_lhs;
}

template <class EqualF>
constexpr cc::string_view cc::string_view::matching_suffix_with(string_view other, EqualF&& eq) const
{
    return string_view::decompose_matching_suffix(*this, other, static_cast<EqualF&&>(eq)).suffix_lhs;
}

template <class EqualF>
constexpr cc::pair<cc::string_view, cc::string_view> cc::string_view::matching_affixes_with(string_view other,
                                                                                            EqualF&& eq) const
{
    auto const decomp = string_view::decompose_matching_affixes(*this, other, static_cast<EqualF&&>(eq));
    return {decomp.prefix_lhs, decomp.suffix_lhs};
}

// ============================================================================
// Member functions modifying this view in place
// ============================================================================

template <class EqualF>
constexpr void cc::string_view::strip_matching_prefix_with(string_view other, EqualF&& eq)
{
    *this = string_view::decompose_matching_prefix(*this, other, static_cast<EqualF&&>(eq)).middle_lhs;
}

template <class EqualF>
constexpr void cc::string_view::strip_matching_suffix_with(string_view other, EqualF&& eq)
{
    *this = string_view::decompose_matching_suffix(*this, other, static_cast<EqualF&&>(eq)).middle_lhs;
}

template <class EqualF>
constexpr void cc::string_view::strip_matching_affixes_with(string_view other, EqualF&& eq)
{
    *this = string_view::decompose_matching_affixes(*this, other, static_cast<EqualF&&>(eq)).middle_lhs;
}

// ============================================================================
// Member functions returning modified copies
// ============================================================================

template <class EqualF>
constexpr cc::string_view cc::string_view::stripped_matching_prefix_with(string_view other, EqualF&& eq) const
{
    return string_view::decompose_matching_prefix(*this, other, static_cast<EqualF&&>(eq)).middle_lhs;
}

template <class EqualF>
constexpr cc::string_view cc::string_view::stripped_matching_suffix_with(string_view other, EqualF&& eq) const
{
    return string_view::decompose_matching_suffix(*this, other, static_cast<EqualF&&>(eq)).middle_lhs;
}

template <class EqualF>
constexpr cc::string_view cc::string_view::stripped_matching_affixes_with(string_view other, EqualF&& eq) const
{
    return string_view::decompose_matching_affixes(*this, other, static_cast<EqualF&&>(eq)).middle_lhs;
}
