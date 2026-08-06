#pragma once

#include <clean-core/container/impl/allocating_container.hh>
#include <clean-core/fwd.hh>
#include <clean-core/memory/allocation.hh>
#include <clean-core/string/string_view.hh>

#include <cstring>
#include <type_traits>

/// Owning UTF-8 byte string with small-string optimization (SSO).
/// Content up to small_capacity bytes lives inline with no allocation; longer content uses heap storage via
/// cc::allocation<char>.
/// size() counts bytes rather than codepoints, and embedded '\0' bytes are allowed.
/// data() is contiguous but NOT null-terminated — c_str_materialize() is the explicit C-interop path.
///
/// A pointer, reference or string_view into a string is valid until the next non-const operation on it.
/// c_str_materialize() is itself non-const and may reallocate, so it invalidates like any mutation.
///
/// The memory resource is sticky across every operation on one string, including transitions between
/// inline and heap storage.
/// Copying is the exception — see the copy constructor and copy assignment.
///
/// Full Unicode semantics — grapheme clusters, codepoint iteration, normalization — are a non-goal.
///
/// libs/base/clean-core/docs/strings.md owns the storage, lifetime, hashing and conversion contracts.
struct cc::string
{
    // constants
private:
    /// Maximum number of bytes that can be stored inline without heap allocation.
    /// Derived from the heap layout rather than hardcoded.
    /// The inline buffer fills the space before the custom_resource pointer, which data_small must alias for
    /// the SSO tag bit, minus one byte for the size tag.
    /// That is 39 bytes on 64-bit and fewer where pointers are smaller, so read this constant rather than
    /// assuming a number.
    static constexpr isize small_capacity = isize(offsetof(allocation<char>, custom_resource)) - 1;

    // factories
public:
    /// Creates a string by copying the contents of a string_view.
    /// Equivalent to the string_view constructor, and exists so the factories cover the same ground.
    [[nodiscard]] static string create_copy_of(string_view source, memory_resource const* resource = nullptr)
    {
        return string(source.data(), source.size(), resource);
    }

    /// Creates a string filled with size copies of the given character.
    /// Precondition: size >= 0.
    [[nodiscard]] static string create_filled(isize size, char value, memory_resource const* resource = nullptr)
    {
        CC_ASSERT(size >= 0, "string size must be non-negative");

        string result;
        result.initialize_small_empty(resource);
        result.resize_to_filled(size, value);
        return result;
    }

    /// Creates a string with uninitialized storage for size bytes.
    /// The caller must write every byte before reading any of them, so use this only when you will overwrite
    /// all of them immediately.
    /// Precondition: size >= 0.
    [[nodiscard]] static string create_uninitialized(isize size, memory_resource const* resource = nullptr)
    {
        CC_ASSERT(size >= 0, "string size must be non-negative");

        string result;
        result.initialize_small_empty(resource);
        result.resize_to_uninitialized(size);
        return result;
    }

    /// Creates a string from an existing allocation<char>, which must already hold valid bytes in its live
    /// range.
    /// Always uses heap mode, even for content that would fit inline, so that the allocation survives for
    /// sharing and zero-copy interop.
    [[nodiscard]] static string create_from_allocation(allocation<char> data)
    {
        string result;
        // Force heap mode by constructing data_heap from allocation
        new (&result._data.heap) data_heap();
        result._data.heap._data = cc::move(data);
        return result;
    }

    /// Creates an empty string with room to grow to capacity bytes without reallocating.
    /// capacity is a lower bound: a heap allocation is rounded up to the allocator's alignment, so
    /// capacity_back() may exceed what was asked for.
    /// Precondition: capacity >= 0.
    [[nodiscard]] static string create_with_capacity(isize capacity, memory_resource const* resource = nullptr)
    {
        CC_ASSERT(capacity >= 0, "capacity must be non-negative");

        string result;
        result.initialize_small_empty(resource);
        result.reserve_back(capacity); // stays inline while capacity fits, else materializes to heap
        return result;
    }

    /// Creates a null-terminated copy of a string_view.
    /// Allocates room for size + 1, copies the source, and writes a terminating '\0' that is NOT counted in
    /// size().
    /// c_str_if_terminated() is guaranteed to succeed on the result.
    /// Equivalent to create_copy_of followed by c_str_materialize, but sizes one allocation for the
    /// terminator up front rather than possibly reallocating for it later.
    [[nodiscard]] static string create_copy_c_str_materialized(string_view source,
                                                               memory_resource const* resource = nullptr)
    {
        auto result = create_with_capacity(source.size() + 1, resource);

        if (result.is_small())
            result._data.small.size = static_cast<u8>(source.size());
        else
            result._data.heap._data.obj_end = result._data.heap._data.obj_start + source.size();

        if (!source.empty())
            std::memcpy(result.data(), source.data(), source.size());
        result.data()[source.size()] = '\0';

        return result;
    }

    // constructors
public:
    /// Constructs an empty string, inline and with no allocation.
    string() { initialize_small_empty(nullptr); }

    /// Constructing from nullptr is a compile error rather than a runtime check.
    /// Use the default constructor for an empty string.
    string(nullptr_t) = delete;

    /// Constructs a string from a single character, always inline.
    explicit string(char c, memory_resource const* resource = nullptr)
    {
        initialize_small_empty(resource);
        _data.small.data[0] = c;
        _data.small.size = 1;
    }

    /// Constructs a string from [ptr, ptr+size).
    /// Precondition: size >= 0, and ptr must not be null unless size == 0.
    explicit string(char const* ptr, isize size, memory_resource const* resource = nullptr)
    {
        CC_ASSERT(size >= 0, "string size must be non-negative");
        CC_ASSERT(ptr != nullptr || size == 0, "null pointer only allowed for empty range");

        if (size <= small_capacity)
        {
            initialize_small_empty(resource);
            if (size > 0)
                std::memcpy(_data.small.data, ptr, size);
            _data.small.size = static_cast<u8>(size);
        }
        else
        {
            initialize_heap_from_data(ptr, size, resource);
        }
    }

    /// Constructs a string from [begin, end).
    /// Precondition: begin <= end, and begin must not be null unless begin == end.
    explicit string(char const* begin, char const* end, memory_resource const* resource = nullptr)
    {
        CC_ASSERT(begin <= end, "invalid pointer range");
        CC_ASSERT(begin != nullptr || begin == end, "null pointer only allowed for empty range");

        auto const size = end - begin;
        if (size <= small_capacity)
        {
            initialize_small_empty(resource);
            if (size > 0)
                std::memcpy(_data.small.data, begin, size);
            _data.small.size = static_cast<u8>(size);
        }
        else
        {
            initialize_heap_from_data(begin, size, resource);
        }
    }

    /// Constructs a string from a null-terminated C string, whose length is computed by scanning for '\0'.
    /// Precondition: cstr must not be nullptr.
    string(char const* cstr, memory_resource const* resource = nullptr)
    {
        CC_ASSERT(cstr != nullptr, "use default constructor for empty string instead of nullptr");

        auto const len = isize(std::strlen(cstr));
        if (len <= small_capacity)
        {
            initialize_small_empty(resource);
            if (len > 0)
                std::memcpy(_data.small.data, cstr, len);
            _data.small.size = u8(len);
        }
        else
        {
            initialize_heap_from_data(cstr, len, resource);
        }
    }

    /// Constructs a string from any container providing .data() and .size().
    /// This conversion is implicit, so passing a string_view where a string is expected silently copies.
    template <class Container>
        requires requires(Container&& c) {
            { c.data() } -> std::convertible_to<char const*>;
            { c.size() } -> std::convertible_to<isize>;
        }
    string(Container&& c, memory_resource const* resource = nullptr)
    {
        auto const* const data_ptr = c.data();
        auto const data_size = static_cast<isize>(c.size());

        if (data_size <= small_capacity)
        {
            initialize_small_empty(resource);
            if (data_size > 0)
                std::memcpy(_data.small.data, data_ptr, data_size);
            _data.small.size = static_cast<u8>(data_size);
        }
        else
        {
            initialize_heap_from_data(data_ptr, data_size, resource);
        }
    }

    /// Destroys the string, freeing the heap allocation if there is one.
    ~string()
    {
        if (!is_small())
            _data.heap.~data_heap();
    }

    // copy
    /// Deep-copies a string.
    /// Always adopts the source's memory resource.
    string(string const& rhs)
    {
        if (rhs.is_small()) [[likely]]
        {
            _data.blocks = rhs._data.blocks;
        }
        else
        {
            initialize_heap_from_data(rhs.data(), rhs.size(), rhs.resource());
        }
    }

    /// Deep-copy assignment, self-assignment safe.
    /// Which memory resource survives depends on rhs's storage: an inline rhs is block-copied, resource word
    /// included, while a heap rhs allocates from this string's resource.
    string& operator=(string const& rhs)
    {
        if (this != &rhs)
        {
            if (!is_small())
                _data.heap.~data_heap();

            if (rhs.is_small()) [[likely]]
            {
                _data.blocks = rhs._data.blocks;
            }
            else
            {
                initialize_heap_from_data(rhs.data(), rhs.size(), resource());
            }
        }
        return *this;
    }

    // move
    /// Move constructor, leaving rhs empty and inline.
    string(string&& rhs) noexcept
    {
        _data.blocks = rhs._data.blocks;
        rhs.initialize_small_empty(rhs._data.small.custom_resource);
    }

    /// Move assignment, self-assignment safe, leaving rhs empty and inline.
    string& operator=(string&& rhs) noexcept
    {
        if (this != &rhs)
        {
            if (!is_small())
                _data.heap.~data_heap();

            _data.blocks = rhs._data.blocks;
            rhs.initialize_small_empty(rhs._data.small.custom_resource);
        }
        return *this;
    }

    // queries
public:
    /// Returns the number of bytes in the string.
    /// Bytes, not codepoints or grapheme clusters; embedded '\0' bytes are counted.
    [[nodiscard]] isize size() const
    {
        if (is_small()) [[likely]]
            return _data.small.size;
        else
            return _data.heap.size();
    }

    /// Returns true if the string contains no bytes.
    [[nodiscard]] bool empty() const { return size() == 0; }

    /// Returns a pointer to the first byte, inline or heap depending on the storage mode.
    /// The bytes are contiguous but NOT null-terminated — use c_str_materialize() for a C API.
    [[nodiscard]] char const* data() const
    {
        if (is_small()) [[likely]]
            return _data.small.data;
        else
            return _data.heap.data();
    }

    /// Returns a mutable pointer to the first byte, for in-place modification.
    /// The bytes are contiguous but NOT null-terminated.
    [[nodiscard]] char* data()
    {
        if (is_small()) [[likely]]
            return _data.small.data;
        else
            return _data.heap.data();
    }

    /// Returns the byte at index i.
    /// Precondition: 0 <= i < size().
    [[nodiscard]] char const& operator[](isize i) const
    {
        CC_ASSERT(0 <= i && i < size(), "index out of bounds");
        return data()[i];
    }

    /// Returns a mutable reference to the byte at index i.
    /// Precondition: 0 <= i < size().
    [[nodiscard]] char& operator[](isize i)
    {
        CC_ASSERT(0 <= i && i < size(), "index out of bounds");
        return data()[i];
    }

    /// Returns the first byte.
    /// Precondition: !empty().
    [[nodiscard]] char front() const { return string_view(*this).front(); }

    /// Returns the last byte.
    /// Precondition: !empty().
    [[nodiscard]] char back() const { return string_view(*this).back(); }

    // span access
public:
    /// The stored chars as a span (no trailing terminator).
    [[nodiscard]] cc::span<char const> as_span() const { return cc::span<char const>(data(), size()); }
    /// The stored chars as a mutable span (no trailing terminator).
    [[nodiscard]] cc::span<char> as_mutable_span() { return cc::span<char>(data(), size()); }
    /// The stored chars as immutable raw bytes.
    [[nodiscard]] cc::span<cc::byte const> as_bytes() const { return as_span().as_bytes(); }
    /// The stored chars as mutable raw bytes.
    [[nodiscard]] cc::span<cc::byte> as_mutable_bytes() { return as_mutable_span().as_mutable_bytes(); }

    // string_view read forwarding
public:
    /// Lexicographically compares with another view, returning <0, 0 or >0.
    /// Two cc::strings have no relational operators, so this is how you order them.
    [[nodiscard]] int compare(string_view other) const { return string_view(*this).compare(other); }

    /// Finds the first occurrence of substring at or after pos, or -1.
    /// Naive search: O(size() * substring.size()).
    /// Precondition: 0 <= pos <= size().
    [[nodiscard]] isize find(string_view substring, isize pos = 0) const
    {
        return string_view(*this).find(substring, pos);
    }

    /// Finds the first occurrence of c at or after pos, or -1.
    /// Precondition: 0 <= pos <= size().
    [[nodiscard]] isize find(char c, isize pos = 0) const { return string_view(*this).find(c, pos); }

    /// Finds the last occurrence of substring at or before pos, or -1.
    /// A pos of -1 searches from the end.
    [[nodiscard]] isize rfind(string_view substring, isize pos = -1) const
    {
        return string_view(*this).rfind(substring, pos);
    }

    /// Finds the last occurrence of c at or before pos (-1 = from the end), or -1.
    [[nodiscard]] isize rfind(char c, isize pos = -1) const { return string_view(*this).rfind(c, pos); }

    // substring operations
public:
    /// Returns a non-owning view [offset, size()) into this string, invalidated like any other pointer into
    /// it.
    /// Precondition: 0 <= offset <= size().
    [[nodiscard]] string_view subview(isize offset) const { return string_view(*this).subview(offset); }

    /// Returns a non-owning view [r.offset, r.offset + r.size) into this string.
    [[nodiscard]] string_view subview(offset_size r) const { return string_view(*this).subview(r); }

    /// Returns a non-owning view [r.start, r.end) into this string.
    [[nodiscard]] string_view subview(start_end r) const { return string_view(*this).subview(r); }

    /// Returns an owning copy of [offset, size()).
    /// Precondition: 0 <= offset <= size().
    [[nodiscard]] string substring(isize offset) const { return string(subview(offset)); }

    /// Returns an owning copy of [r.offset, r.offset + r.size).
    [[nodiscard]] string substring(offset_size r) const { return string(subview(r)); }

    /// Returns an owning copy of [r.start, r.end).
    [[nodiscard]] string substring(start_end r) const { return string(subview(r)); }

    // C interop
public:
    /// Materializes a null-terminated C string on demand and returns it.
    /// Writes a '\0' immediately after the content, which does NOT change size().
    /// May allocate, and may move an inline string to the heap.
    ///
    /// The returned pointer is valid only until the next non-const operation, so call this immediately
    /// before the C call and never store the result:
    ///   some_c_function(str.c_str_materialize());
    [[nodiscard]] char const* c_str_materialize()
    {
        if (is_small()) [[likely]]
        {
            if (_data.small.size < small_capacity) [[likely]]
            {
                _data.small.data[_data.small.size] = '\0';
                return _data.small.data;
            }
            else
            {
                materialize_heap(1);
                _data.heap.data()[_data.heap.size()] = '\0';
                return _data.heap.data();
            }
        }
        else
        {
            _data.heap.reserve_back(1);
            _data.heap.data()[_data.heap.size()] = '\0';
            return _data.heap.data();
        }
    }

    /// Returns a C string pointer when the content already happens to be null-terminated, nullptr otherwise.
    /// A const inspection: it checks that capacity reaches past size() and that the byte at size() is '\0'.
    /// Strings from create_copy_c_str_materialized() always succeed here.
    ///
    /// The returned pointer is valid only until the next non-const operation.
    ///
    /// Try this before materializing, which matters most for a string you only hold by const reference,
    /// where materializing means copying:
    ///   if (auto* cstr = str.c_str_if_terminated())
    ///       some_c_function(cstr);
    ///   else
    ///       some_c_function(string::create_copy_c_str_materialized(str).c_str_if_terminated());
    [[nodiscard]] char const* c_str_if_terminated() const
    {
        if (is_small()) [[likely]]
        {
            if (_data.small.size < small_capacity && //
                _data.small.data[_data.small.size] == '\0')
                return _data.small.data;
        }
        else
        {
            if ((cc::byte const*)_data.heap._data.obj_end < _data.heap._data.alloc_end && //
                *_data.heap._data.obj_end == '\0')
                return _data.heap.data();
        }

        return nullptr;
    }

    // mutating operations
public:
    /// Appends a single byte.
    /// Moves to the heap when the inline buffer is full, and may reallocate when heap capacity runs out.
    /// Amortized O(1).
    void push_back(char c)
    {
        if (is_small()) [[likely]]
        {
            if (_data.small.size < small_capacity) [[likely]]
            {
                _data.small.data[_data.small.size] = c;
                _data.small.size++;
                return;
            }
            else
                materialize_heap(1);
        }

        _data.heap.push_back(c);
    }

    /// Appends the contents of a string_view.
    /// Stays inline while the result fits, and otherwise moves to or grows the heap allocation.
    void append(string_view sv)
    {
        if (sv.empty())
            return;

        if (is_small()) [[likely]]
        {
            auto const new_size = _data.small.size + sv.size();
            if (new_size <= small_capacity)
            {
                std::memcpy(_data.small.data + _data.small.size, sv.data(), sv.size());
                _data.small.size = u8(new_size);
                return;
            }
            else
                materialize_heap(sv.size());
        }

        auto const old_size = _data.heap.size();
        _data.heap.resize_to_uninitialized(old_size + sv.size());
        std::memcpy(_data.heap.data() + old_size, sv.data(), sv.size());
    }

    /// Appends a single character.
    void append(char c) { push_back(c); }

    /// Appends the contents of a string_view and returns *this for chaining.
    string& operator+=(string_view sv)
    {
        append(sv);
        return *this;
    }

    /// Appends a single character and returns *this for chaining.
    string& operator+=(char c)
    {
        append(c);
        return *this;
    }

    /// Appends formatted output, equivalent to cc::format_append(*this, fmt, args...).
    /// The format string is validated against the argument types at compile time.
    ///
    /// Only declared here: the definition lives in <clean-core/string/format.hh>, which you must include to
    /// call this, since string.hh deliberately does not pull in the format machinery.
    template <class... Args>
    void appendf(format_string<std::type_identity_t<Args>...> fmt, Args&&... args);

    /// Concatenates a string and a string_view, taking the left side by value.
    [[nodiscard]] friend string operator+(string lhs, string_view rhs)
    {
        lhs.append(rhs);
        return lhs;
    }

    /// Concatenates a string and a character, taking the left side by value.
    [[nodiscard]] friend string operator+(string lhs, char rhs)
    {
        lhs.append(rhs);
        return lhs;
    }

    /// Clears the string to empty without deallocating.
    /// Capacity and storage mode are both preserved.
    void clear()
    {
        if (is_small()) [[likely]]
        {
            _data.small.size = 0;
        }
        else
        {
            _data.heap.clear();
        }
    }

    // capacity queries
public:
    /// Number of bytes that can be appended at the back without reallocation.
    /// In SSO mode this is the unused inline room (small_capacity - size()).
    [[nodiscard]] isize capacity_back() const
    {
        if (is_small()) [[likely]]
            return small_capacity - _data.small.size;
        else
            return _data.heap.capacity_back();
    }

    /// Number of bytes of unused capacity before the content (front slack).
    /// Always 0 in SSO mode (the inline layout has no front offset); grows only via reserve_front on the heap.
    [[nodiscard]] isize capacity_front() const
    {
        if (is_small()) [[likely]]
            return 0;
        else
            return _data.heap.capacity_front();
    }

    // resize and capacity
public:
    /// Resizes to new_size bytes, leaving newly added bytes uninitialized and preserving existing ones.
    /// Growing extends at the back; shrinking drops trailing bytes.
    /// Stays inline while new_size fits, and otherwise moves to the heap.
    /// Only shrink_to_fit() ever moves back, so this never demotes.
    /// Precondition: new_size >= 0.
    void resize_to_uninitialized(isize new_size)
    {
        CC_ASSERT(new_size >= 0, "string size must be non-negative");

        if (is_small()) [[likely]]
        {
            if (new_size <= small_capacity)
            {
                _data.small.size = u8(new_size); // grow: new bytes uninitialized; shrink: trailing bytes dropped
                return;
            }
            materialize_heap(new_size - _data.small.size);
        }

        _data.heap.resize_to_uninitialized(new_size);
    }

    /// Resizes to new_size bytes, filling newly added bytes with value and preserving existing ones.
    /// Growing extends at the back; shrinking drops trailing bytes.
    /// Precondition: new_size >= 0.
    void resize_to_filled(isize new_size, char value)
    {
        CC_ASSERT(new_size >= 0, "string size must be non-negative");

        if (is_small()) [[likely]]
        {
            if (new_size <= small_capacity)
            {
                for (isize i = _data.small.size; i < new_size; ++i) // empty loop when shrinking
                    _data.small.data[i] = value;
                _data.small.size = u8(new_size);
                return;
            }
            materialize_heap(new_size - _data.small.size);
        }

        _data.heap.resize_to_filled(new_size, value);
    }

    /// Resizes to new_size bytes, zero-filling any newly added bytes.
    /// Growing extends at the back; shrinking drops trailing bytes.
    /// Existing bytes are preserved.
    /// Precondition: new_size >= 0.
    void resize_to_defaulted(isize new_size) { resize_to_filled(new_size, char()); }

    /// Shrinks to new_size bytes by dropping the trailing bytes.
    /// Does not reallocate or change the storage mode.
    /// Precondition: 0 <= new_size <= size().
    void resize_down_to(isize new_size)
    {
        CC_ASSERT(0 <= new_size && new_size <= size(), "resize_down_to: new_size must be in [0, size()]");

        if (is_small()) [[likely]]
            _data.small.size = u8(new_size);
        else
            _data.heap.resize_down_to(new_size);
    }

    /// Discards all current content, then resizes to new_size bytes left uninitialized.
    /// Unlike resize_*, existing bytes are NOT preserved when growing.
    /// Precondition: new_size >= 0.
    void clear_resize_to_uninitialized(isize new_size)
    {
        CC_ASSERT(new_size >= 0, "string size must be non-negative");

        if (is_small()) [[likely]]
        {
            if (new_size <= small_capacity)
            {
                _data.small.size = u8(new_size);
                return;
            }
            // content is about to be discarded, so the inline copy inside materialize_heap is harmless
            materialize_heap(new_size - _data.small.size);
        }

        _data.heap.clear_resize_to_uninitialized(new_size);
    }

    /// Discards all current content, then resizes to new_size bytes all filled with value.
    /// Unlike resize_*, existing bytes are NOT preserved when growing.
    /// Precondition: new_size >= 0.
    void clear_resize_to_filled(isize new_size, char value)
    {
        CC_ASSERT(new_size >= 0, "string size must be non-negative");

        if (is_small()) [[likely]]
        {
            if (new_size <= small_capacity)
            {
                for (isize i = 0; i < new_size; ++i)
                    _data.small.data[i] = value;
                _data.small.size = u8(new_size);
                return;
            }
            materialize_heap(new_size - _data.small.size);
        }

        _data.heap.clear_resize_to_filled(new_size, value);
    }

    /// Discards all current content, then resizes to new_size zero-filled bytes.
    /// Precondition: new_size >= 0.
    void clear_resize_to_defaulted(isize new_size) { clear_resize_to_filled(new_size, char()); }

    /// Ensures at least count MORE bytes can be appended at the back without reallocating.
    /// count is a delta on top of the current size, not an absolute capacity.
    /// A no-op while the string stays inline, since SSO already reserves small_capacity bytes.
    /// Growth is exponential only once the string is on the heap; the move from inline to heap allocates
    /// just what count asks for.
    /// Precondition: count >= 0.
    void reserve_back(isize count)
    {
        CC_ASSERT(count >= 0, "reserve count must be non-negative");

        if (is_small()) [[likely]]
        {
            if (_data.small.size + count <= small_capacity)
                return;
            materialize_heap(count);
        }

        _data.heap.reserve_back(count);
    }

    /// Ensures at least count MORE bytes can be appended at the back without reallocating.
    /// Like reserve_back, but allocates exactly the needed space with no exponential slack.
    /// Precondition: count >= 0.
    void reserve_back_exact(isize count)
    {
        CC_ASSERT(count >= 0, "reserve count must be non-negative");

        if (is_small()) [[likely]]
        {
            if (_data.small.size + count <= small_capacity)
                return;
            materialize_heap(count);
        }

        _data.heap.reserve_back_exact(count);
    }

    /// Ensures at least count MORE bytes of unused capacity BEFORE the content (front slack).
    /// No string operation consumes front slack today; it survives back-growth until shrink_to_fit().
    ///
    /// An inline string always moves to the heap here, since SSO cannot represent a front offset.
    /// The new allocation holds small_capacity + count bytes, placed so that capacity_front() == count and
    /// the back capacity matches what SSO had.
    /// Precondition: count >= 0.
    void reserve_front(isize count)
    {
        CC_ASSERT(count >= 0, "reserve count must be non-negative");

        if (is_small()) [[likely]]
        {
            if (count == 0)
                return;
            // keep the SSO-equivalent back room (small_capacity - size) behind the content
            materialize_heap_front(count, small_capacity - _data.small.size);
            return;
        }

        _data.heap.reserve_front(count);
    }

    /// Ensures at least count MORE bytes of front slack, allocating exactly the needed space.
    /// A small string materializes to heap with capacity_front() == count and no back slack.
    /// Precondition: count >= 0.
    void reserve_front_exact(isize count)
    {
        CC_ASSERT(count >= 0, "reserve count must be non-negative");

        if (is_small()) [[likely]]
        {
            if (count == 0)
                return;
            materialize_heap_front(count, 0);
            return;
        }

        _data.heap.reserve_front_exact(count);
    }

    /// Releases excess capacity so the allocation fits the current content.
    /// A no-op in inline mode, which is already minimal, and for an already-tight heap allocation.
    /// A heap string whose content fits inline always drops back to SSO, freeing the heap allocation
    /// entirely; otherwise the heap block is tightened in place.
    /// This is the only operation that returns a heap string to inline storage.
    /// May invalidate pointers.
    void shrink_to_fit()
    {
        if (is_small()) [[likely]]
            return;

        // Fitting inline always wins over any heap block, so this must come before any heap-tightness test.
        // With a 128-byte alloc_alignment, some targets' cache line, a block can read as already tight for
        // the current size and still be demotable.
        if (size() <= small_capacity)
        {
            demote_to_small();
            return;
        }

        // Otherwise tighten the heap block in place; data_heap::shrink_to_fit is itself a no-op when the allocation is already tight.
        _data.heap.shrink_to_fit();
    }

    /// Replaces every occurrence of from with to, in place.
    /// Returns the number of replacements.
    isize replace_all(char from, char to);

    /// Replaces every non-overlapping occurrence of from with to (scanning left to right).
    /// An empty from matches nothing and the string is left unchanged (returns 0).
    /// Returns the number of replacements.
    /// May reallocate.
    /// Complexity: O(size() * from.size()).
    isize replace_all(string_view from, string_view to);

    /// Replaces the first occurrence of from with to.
    /// Returns true if a replacement was made.
    bool replace_first(char from, char to);

    /// Replaces the first occurrence of from with to (no-op for empty from).
    /// Returns true if replaced.
    bool replace_first(string_view from, string_view to);

    /// Replaces the last occurrence of from with to.
    /// Returns true if a replacement was made.
    bool replace_last(char from, char to);

    /// Replaces the last occurrence of from with to (no-op for empty from).
    /// Returns true if replaced.
    bool replace_last(string_view from, string_view to);

    /// Replaces the bytes [r.offset, r.offset + r.size) with the contents of with.
    /// Precondition: r designates a valid range of this string (r.size >= 0, r.offset + r.size <= size()).
    /// May reallocate.
    /// Complexity: O(size() + with.size()).
    void replace(offset_size r, string_view with);

    /// Replaces the bytes [r.start, r.end) with the contents of with.
    /// Precondition: r designates a valid range of this string (r.end >= r.start, r.end <= size()).
    /// May reallocate.
    /// Complexity: O(size() + with.size()).
    void replace(start_end r, string_view with);

    // comparisons
public:
    /// Compares against anything convertible to string_view for equality of size and content.
    template <class S>
    [[nodiscard]] bool operator==(S&& rhs) const
        requires std::convertible_to<S, string_view>
    {
        return string_view(*this) == string_view(cc::forward<S>(rhs));
    }

    /// Compares against another string for equality of size and content.
    [[nodiscard]] bool operator==(string const& rhs) const { return string_view(*this) == string_view(rhs); }

    /// Structural hash over the bytes; delegates to string_view so equal content hashes equally regardless of SSO vs heap storage (and matches a string_view of the same content).
    [[nodiscard]] friend u64 hash(string const& s) { return hash(string_view(s)); }

    /// Returns true if the string begins with prefix.
    [[nodiscard]] bool starts_with(string_view prefix) const { return string_view(*this).starts_with(prefix); }

    /// Returns true if the string ends with suffix.
    [[nodiscard]] bool ends_with(string_view suffix) const { return string_view(*this).ends_with(suffix); }

    /// Returns true if sv occurs anywhere in the string.
    /// Naive search: O(size() * sv.size()).
    [[nodiscard]] bool contains(string_view sv) const { return string_view(*this).contains(sv); }

    /// Returns true if c occurs anywhere in the string.
    [[nodiscard]] bool contains(char c) const { return string_view(*this).contains(c); }

    // storage mode
public:
    /// True while the content is stored inline and no heap allocation exists.
    /// The flag is the low bit of the custom_resource word: set means inline, clear means heap.
    /// Only shrink_to_fit() turns a heap string back into an inline one.
    [[nodiscard]] bool is_small() const { return (reinterpret_cast<uintptr_t>(_data.small.custom_resource) & 1) != 0; }

    // helpers
private:
    /// Returns the memory resource, with the inline-mode tag bit removed.
    /// nullptr means the default global memory resource.
    [[nodiscard]] memory_resource const* resource() const { return remove_small_tag(_data.small.custom_resource); }

    /// Tags a memory resource pointer to mark inline mode by setting its low bit.
    /// Safe because memory_resource pointers are always aligned.
    [[nodiscard]] static memory_resource const* add_small_tag(memory_resource const* r)
    {
        return reinterpret_cast<memory_resource const*>(reinterpret_cast<uintptr_t>(r) | 1);
    }

    /// Recovers the original resource pointer, or nullptr, by clearing the tag bit.
    [[nodiscard]] static memory_resource const* remove_small_tag(memory_resource const* r)
    {
        return reinterpret_cast<memory_resource const*>(reinterpret_cast<uintptr_t>(r) & ~uintptr_t(1));
    }

    /// Initializes the union in inline mode, with size 0 and a tagged resource pointer.
    void initialize_small_empty(memory_resource const* resource)
    {
        _data.small.size = 0;
        _data.small.custom_resource = add_small_tag(resource);
    }

    /// Initializes the union in heap mode, allocating for len bytes and copying them from str.
    /// Precondition: the union must be uninitialized or previously destroyed.
    void initialize_heap_from_data(char const* str, isize len, memory_resource const* resource);

    /// Moves from inline to heap mode, copying the inline content into a new allocation with at least
    /// min_back_capacity bytes of room after it.
    /// Precondition: is_small().
    void materialize_heap(isize min_back_capacity);

    /// Moves from inline to heap mode, placing the content with front_capacity unused bytes before it and
    /// back_capacity unused bytes after it, so that capacity_front() == front_capacity afterwards.
    /// Precondition: is_small() and front_capacity >= 0 and back_capacity >= 0.
    void materialize_heap_front(isize front_capacity, isize back_capacity);

    /// Moves from heap back to inline mode, copying the live bytes into the inline buffer and freeing the
    /// allocation.
    /// The memory resource is preserved.
    /// Precondition: !is_small() and size() <= small_capacity.
    void demote_to_small();

    // data member
private:
    struct data_heap : cc::allocating_container<char, data_heap>
    {
        static constexpr bool uses_capacity_front = true;
        friend struct cc::string;
    };

    // data_small overlays data_heap: its custom_resource must alias the heap allocation's custom_resource
    // (so the SSO tag bit lives in the same word), and alignas(data_heap) pads it to data_heap's size and
    // alignment so the three union members stay congruent on any pointer size.
    struct alignas(data_heap) data_small
    {
        char data[small_capacity];
        u8 size;
        memory_resource const* custom_resource;
    };

    // Raw u64 view of the union for whole-object bit-copies; sized to the union (= data_heap), not hardcoded.
    static_assert(sizeof(data_heap) % sizeof(u64) == 0, "data_heap must be a whole number of u64 blocks");
    struct data_blocks
    {
        u64 blocks[sizeof(data_heap) / sizeof(u64)];
    };

    static_assert(sizeof(data_heap) == sizeof(data_small), "inconsistent data layout");
    static_assert(sizeof(data_heap) == sizeof(data_blocks), "inconsistent data layout");
    static_assert(offsetof(allocation<char>, custom_resource) == offsetof(data_small, custom_resource),
                  "inconsistent data layout");

    union data // NOLINT
    {
        data_blocks blocks;
        data_heap heap;
        data_small small;

        data() {} // NOLINT
        ~data() {}
    } _data;
};
