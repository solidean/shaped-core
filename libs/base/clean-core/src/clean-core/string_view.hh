#pragma once

#include <clean-core/assert.hh>
#include <clean-core/char_predicates.hh>
#include <clean-core/fwd.hh>
#include <clean-core/pair.hh>

/// Non-owning view over a contiguous sequence of char, interpreted as UTF-8.
/// Stores char const* data and isize size.
/// Trivially copyable.
/// Does not own the underlying memory; caller must ensure the referenced data outlives the string_view.
///
/// WARNING: string_view does NOT guarantee a trailing null terminator.
/// The viewed string may or may not have a '\0' after the last character.
/// Use data() carefully - it is NOT necessarily null-terminated.
/// If you need a null-terminated string, you must copy to a null-terminated container.
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
    /// WARNING: No null terminator is required or guaranteed.
    constexpr explicit string_view(char const* ptr, isize size) : _data(ptr), _size(size)
    {
        CC_ASSERT(size >= 0, "string_view size must be non-negative");
        CC_ASSERT(ptr != nullptr || size == 0, "null pointer only allowed for empty range");
    }

    /// Creates a string_view viewing [begin, end).
    /// Precondition: begin <= end, and begin must not be null unless begin == end.
    /// WARNING: No null terminator is required or guaranteed.
    constexpr explicit string_view(char const* begin, char const* end) : _data(begin), _size(end - begin)
    {
        CC_ASSERT(begin <= end, "invalid pointer range");
        CC_ASSERT(begin != nullptr || begin == end, "null pointer only allowed for empty range");
    }

    /// Creates a string_view from a null-terminated C string.
    /// Computes length by searching for '\0'.
    /// The resulting view does NOT include the null terminator.
    /// Precondition: cstr must not be null.
    constexpr string_view(char const* cstr)
    {
        CC_ASSERT(cstr != nullptr, "string_view cannot be constructed from nullptr");
        _data = cstr;
        _size = compute_length(cstr);
    }

    /// Creates a string_view from a null-terminated C string literal.
    /// Deduces size N from array type (includes '\0').
    /// The resulting view excludes the null terminator: size() == N - 1.
    ///
    /// CAUTION: This constructor assumes the array has a null terminator at position N-1.
    /// If you have a local char buffer that may not have a terminator, or contains a shorter
    /// string than the full buffer size, you MUST use the explicit string_view(ptr, size) constructor instead.
    ///
    /// Example:
    ///   char buf[100] = "hello";  // only 5 chars + '\0', rest is uninitialized
    ///   auto sv1 = string_view(buf);        // WRONG: creates view of size 99
    ///   auto sv2 = string_view(buf, 5);     // CORRECT: creates view of size 5
    template <isize N>
    constexpr string_view(char const (&arr)[N]) : _data(arr), _size(N - 1)
    {
        static_assert(N > 0, "string literal must have at least a null terminator");
    }

    /// Creates a string_view from any container providing .data() and .size().
    /// Requires that .data() converts to char const*.
    /// The string_view does not own the container; the container must outlive the string_view.
    /// Passing a temporary container is safe when the string_view is used immediately (e.g., function argument).
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

    /// Returns a pointer to the underlying contiguous storage.
    /// May be nullptr if the string_view is default-constructed or empty.
    ///
    /// WARNING: The pointed-to data is NOT guaranteed to be null-terminated.
    /// Do NOT assume you can pass data() to C APIs expecting '\0'-terminated strings.
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
    /// Returns the number of bytes in the string_view.
    /// This is the byte length, not the number of UTF-8 code points.
    [[nodiscard]] constexpr isize size() const { return _size; }
    /// Returns true if size() == 0.
    [[nodiscard]] constexpr bool empty() const { return _size == 0; }

    // substring operations
public:
    /// Returns a subview starting at offset with the specified size.
    /// Precondition: offset <= size() && offset + size <= size().
    [[nodiscard]] constexpr string_view subview(isize offset, isize size) const
    {
        CC_ASSERT(offset <= _size, "subview offset out of range");
        CC_ASSERT(offset + size <= _size, "subview range out of bounds");
        return string_view(_data + offset, size);
    }

    /// Returns a subview starting at offset to the end of the string.
    /// Precondition: offset <= size().
    [[nodiscard]] constexpr string_view subview(isize offset) const
    {
        CC_ASSERT(offset <= _size, "subview offset out of range");
        return string_view(_data + offset, _size - offset);
    }

    /// Returns a subview starting at offset with the specified size, clamped to valid bounds.
    /// If offset > size(), returns empty view.
    /// If offset + size > size(), the view is truncated to fit.
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
public:
    // Forward declarations of result types (defined at bottom of file)
    struct decomposed_prefix;
    struct decomposed_suffix;
    struct decomposed_affixes;

    /// Decomposes two string views by finding their common prefix
    ///
    /// Compares characters from the beginning of both views using the provided equality function.
    /// Returns a decomposed_prefix containing the matching prefix parts and the remaining parts.
    ///
    /// The equality function should have signature: bool(char, char)
    /// Default is case-sensitive comparison.
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

    /// Decomposes two string views by finding their common suffix
    ///
    /// Compares characters from the end of both views backwards using the provided equality function.
    /// Returns a decomposed_suffix containing the remaining parts and the matching suffix parts.
    ///
    /// The equality function should have signature: bool(char, char)
    /// Default is case-sensitive comparison.
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

    /// Decomposes two string views by finding their common prefix AND suffix
    ///
    /// First finds the common prefix, then finds the common suffix in the remaining parts.
    /// Returns a decomposed_affixes containing the prefix, middle, and suffix parts.
    ///
    /// The equality function should have signature: bool(char, char)
    /// Default is case-sensitive comparison.
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
    [[nodiscard]] constexpr int compare(string_view other) const
    {
        auto const min_size = _size < other._size ? _size : other._size;
        for (isize i = 0; i < min_size; ++i)
        {
            if (_data[i] != other._data[i])
                return int(_data[i]) - int(other._data[i]);
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
