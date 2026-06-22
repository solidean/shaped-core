#pragma once

#include <clean-core/allocation.hh>
#include <clean-core/fwd.hh>
#include <clean-core/impl/allocating_container.hh>
#include <clean-core/string_view.hh>

#include <cstring>

/// Owning UTF-8 byte string with small-string optimization (SSO).
/// Stores up to 39 bytes inline without allocation; longer strings use heap storage via cc::allocation<char>.
/// size() counts bytes, not codepoints; embedded '\0' bytes are allowed.
/// data() returns contiguous bytes but is NOT null-terminated.
///
/// C interop requires explicit materialization:
/// Use c_str_materialize() to obtain a temporary '\0'-terminated pointer valid only until the next mutation.
/// This design avoids the overhead of maintaining a persistent terminator for all operations.
///
/// Memory resource choice ("custom_resource is sticky") is preserved across all operations, including transitions
/// between SSO and heap. Mutating operations may invalidate pointers and references, as with std::string.
///
/// Performance characteristics:
/// SSO fast paths (≤39 bytes) avoid allocation and branch on size checks.
/// For data-intensive or SIMD-heavy workloads, prefer string_view/span over repeated string operations.
///
/// Non-goal: Full Unicode semantics (grapheme clusters, codepoint iteration).
/// For heavyweight UTF-8 processing, use cc::text/text_view (planned).
struct cc::string
{
    // constants
private:
    /// Maximum number of bytes that can be stored inline without heap allocation.
    /// Computed as 39 bytes (5 * 8 - 1), leaving room for size byte and tagged resource pointer.
    static constexpr isize small_capacity = sizeof(u64) * 5 - 1; // 39 bytes

    // factories
public:
    /// Creates a string by copying the contents of a string_view.
    /// If the content fits in 39 bytes, uses SSO mode (no allocation).
    /// Otherwise, allocates heap storage from the specified memory resource.
    /// Equivalent to the string_view constructor, but provides a consistent factory interface.
    /// Complexity: O(source.size()).
    [[nodiscard]] static string create_copy_of(string_view source, memory_resource const* resource = nullptr)
    {
        return string(source.data(), source.size(), resource);
    }

    /// Creates a string filled with size copies of the given character.
    /// If size <= 39, uses SSO mode (no allocation).
    /// Otherwise, allocates heap storage from the specified memory resource.
    /// Complexity: O(size).
    [[nodiscard]] static string create_filled(isize size, char value, memory_resource const* resource = nullptr)
    {
        CC_ASSERT(size >= 0, "string size must be non-negative");

        string result;
        if (size <= small_capacity)
        {
            result.initialize_small_empty(resource);
            for (isize i = 0; i < size; ++i)
                result._data.small.data[i] = value;
            result._data.small.size = static_cast<u8>(size);
        }
        else
        {
            // Allocate heap storage and fill
            result.initialize_heap_from_data("", 0, resource);
            result._data.heap.resize_to_uninitialized(size);
            for (isize i = 0; i < size; ++i)
                result._data.heap.data()[i] = value;
        }
        return result;
    }

    /// Creates a string with uninitialized storage for size bytes.
    /// The caller is responsible for initializing the bytes before reading them.
    /// If size <= 39, uses SSO mode (no allocation).
    /// Otherwise, allocates heap storage from the specified memory resource.
    /// Use this only when you will immediately overwrite all bytes.
    /// Complexity: O(1) for SSO, O(size) for heap (allocation only, no initialization).
    [[nodiscard]] static string create_uninitialized(isize size, memory_resource const* resource = nullptr)
    {
        CC_ASSERT(size >= 0, "string size must be non-negative");

        string result;
        if (size <= small_capacity)
        {
            result.initialize_small_empty(resource);
            result._data.small.size = static_cast<u8>(size);
            // Note: bytes are uninitialized; caller must fill them
        }
        else
        {
            result.initialize_heap_from_data("", 0, resource);
            result._data.heap.resize_to_uninitialized(size);
        }
        return result;
    }

    /// Creates a string from an existing allocation<char>.
    /// The allocation must already contain valid UTF-8 byte data in its live range.
    /// Always uses heap mode, even if the content would fit in SSO.
    /// This preserves the allocation for efficient memory sharing and zero-copy interop.
    /// Complexity: O(1).
    [[nodiscard]] static string create_from_allocation(allocation<char> data)
    {
        string result;
        // Force heap mode by constructing data_heap from allocation
        new (&result._data.heap) data_heap();
        result._data.heap._data = cc::move(data);
        return result;
    }

    /// Creates an empty string with pre-allocated capacity.
    /// If capacity <= 39, uses SSO mode (no allocation).
    /// Otherwise, allocates heap storage with the specified capacity from the memory resource.
    /// The string is initially empty (size() == 0), but can grow up to capacity without reallocation.
    /// Complexity: O(1). (allocation only, no initialization).
    [[nodiscard]] static string create_with_capacity(isize capacity, memory_resource const* resource = nullptr)
    {
        CC_ASSERT(capacity >= 0, "capacity must be non-negative");

        string result;
        if (capacity <= small_capacity)
        {
            result.initialize_small_empty(resource);
        }
        else
        {
            result.initialize_heap_from_data("", 0, resource);
            result._data.heap.reserve_back(capacity);
        }
        return result;
    }

    /// Creates a null-terminated copy of a string_view.
    /// Allocates capacity for size + 1, copies the source, and writes a terminating '\0'.
    /// The '\0' is written to storage but NOT included in size().
    /// This is semantically equivalent to calling create_copy_of followed by c_str_materialize,
    /// but more efficient as it pre-allocates the terminator space.
    /// Use this when you know you'll need a null-terminated string for C interop.
    /// Guarantees that c_str_if_terminated() will return a valid pointer (not nullptr).
    /// Complexity: O(source.size()).
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
    /// Constructs an empty string with no allocation.
    /// Uses small string optimization (SSO) mode.
    /// The string is ready to use and can grow up to 39 bytes without allocating.
    string() { initialize_small_empty(nullptr); }

    /// Prevent construction from nullptr to avoid ambiguity (compile-time error instead of runtime check).
    /// Use the default constructor string() for an empty string instead.
    string(nullptr_t) = delete;

    /// Constructs a string from a single character.
    /// Always uses SSO mode (no allocation).
    /// Complexity: O(1).
    explicit string(char c, memory_resource const* resource = nullptr)
    {
        initialize_small_empty(resource);
        _data.small.data[0] = c;
        _data.small.size = 1;
    }

    /// Constructs a string from [ptr, ptr+size).
    /// If the content fits in 39 bytes, uses SSO mode (no allocation).
    /// Otherwise, allocates heap storage from the specified memory resource.
    /// Precondition: size >= 0, and ptr must not be null unless size == 0.
    /// Complexity: O(size).
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
    /// If the content fits in 39 bytes, uses SSO mode (no allocation).
    /// Otherwise, allocates heap storage from the specified memory resource.
    /// Precondition: begin <= end, and begin must not be null unless begin == end.
    /// Complexity: O(end - begin).
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

    /// Constructs a string from a null-terminated C string.
    /// If the string fits in 39 bytes, uses SSO mode (no allocation).
    /// Otherwise, allocates heap storage from the specified memory resource.
    /// Precondition: cstr must not be nullptr (use default constructor for empty string).
    /// Complexity: O(n) where n is the length of the string.
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
    /// If the content fits in 39 bytes, uses SSO mode (no allocation).
    /// Otherwise, allocates heap storage from the specified memory resource.
    /// Complexity: O(n) where n is the size of the container.
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

    /// Destroys the string, freeing heap allocation if present.
    /// Small strings (SSO mode) require no cleanup.
    /// Complexity: O(1).
    ~string()
    {
        if (!is_small())
            _data.heap.~data_heap();
    }

    // copy
    /// Deep-copies a string.
    /// If the source is small (SSO), performs a fast block copy.
    /// If the source is heap-allocated, allocates new storage and copies the bytes.
    /// Preserves the source's memory resource.
    /// Complexity: O(n) for heap strings, O(1) for small strings.
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

    /// Deep-copy assignment.
    /// Destroys the current content (if heap-allocated), then copies from rhs.
    /// Preserves this string's memory resource (does not copy allocator).
    /// Self-assignment safe.
    /// Complexity: O(n) for heap strings, O(1) for small strings.
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
    /// Move constructor.
    /// Transfers ownership of rhs's content via block copy.
    /// Leaves rhs in a valid empty state (small mode).
    /// Works for both small and heap strings.
    /// Complexity: O(1).
    string(string&& rhs) noexcept
    {
        _data.blocks = rhs._data.blocks;
        rhs.initialize_small_empty(rhs._data.small.custom_resource);
    }

    /// Move assignment.
    /// Destroys current content (if heap), then transfers ownership from rhs.
    /// Leaves rhs in a valid empty state (small mode).
    /// Self-assignment safe.
    /// Complexity: O(1).
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
    /// Note: This counts bytes, not Unicode codepoints or grapheme clusters.
    /// Embedded '\0' bytes are allowed and counted.
    /// Complexity: O(1).
    [[nodiscard]] isize size() const
    {
        if (is_small()) [[likely]]
            return _data.small.size;
        else
            return _data.heap.size();
    }

    /// Returns true if the string contains no bytes (size() == 0).
    /// Complexity: O(1).
    [[nodiscard]] bool empty() const { return size() == 0; }

    /// Returns a pointer to the first byte of the string.
    /// The data is contiguous but NOT null-terminated.
    /// Use c_str_materialize() if you need a null-terminated string for C APIs.
    /// May return a pointer to internal SSO buffer or heap allocation.
    /// Complexity: O(1).
    [[nodiscard]] char const* data() const
    {
        if (is_small()) [[likely]]
            return _data.small.data;
        else
            return _data.heap.data();
    }

    /// Returns a mutable pointer to the first byte of the string.
    /// The data is contiguous but NOT null-terminated.
    /// Allows in-place modification of individual bytes.
    /// May return a pointer to internal SSO buffer or heap allocation.
    /// Complexity: O(1).
    [[nodiscard]] char* data()
    {
        if (is_small()) [[likely]]
            return _data.small.data;
        else
            return _data.heap.data();
    }

    /// Returns a const reference to the byte at index i.
    /// Precondition: 0 <= i < size().
    /// Complexity: O(1).
    [[nodiscard]] char const& operator[](isize i) const
    {
        CC_ASSERT(0 <= i && i < size(), "index out of bounds");
        return data()[i];
    }

    /// Returns a mutable reference to the byte at index i.
    /// Precondition: 0 <= i < size().
    /// Complexity: O(1).
    [[nodiscard]] char& operator[](isize i)
    {
        CC_ASSERT(0 <= i && i < size(), "index out of bounds");
        return data()[i];
    }

    // C interop
public:
    /// Materializes a null-terminated C string on demand.
    /// Writes a '\0' byte immediately after the string content (does NOT change size()).
    /// May allocate or transition from SSO to heap if needed.
    ///
    /// IMPORTANT: The returned pointer is only valid until the next non-const operation.
    /// This is an explicit FFI boundary operation, not a persistent invariant.
    ///
    /// Usage pattern: Call immediately before passing to C API, do not store the pointer.
    /// Example: some_c_function(str.c_str_materialize());
    ///
    /// Complexity: O(1) if capacity exists, O(n) if reallocation needed.
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

    /// Returns a C string pointer if already null-terminated, nullptr otherwise.
    /// This is a const inspection that does NOT modify the string.
    /// Checks if capacity > size and if the byte at position size() is already '\0'.
    ///
    /// Strings created with create_copy_c_str_materialized() are guaranteed to succeed.
    ///
    /// IMPORTANT: The returned pointer is only valid until the next non-const operation.
    ///
    /// Usage pattern: Optimistically check before materializing to avoid allocation.
    /// Most effective with const strings, where materialization requires a copy.
    /// Example:
    ///   if (auto* cstr = str.c_str_if_terminated())
    ///       some_c_function(cstr);
    ///   else
    ///       some_c_function(string::create_copy_c_str_materialized(str).c_str_if_terminated());
    ///
    /// Complexity: O(1).
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
    /// Appends a single byte to the end of the string.
    /// If small and at capacity (39 bytes), transitions to heap mode.
    /// May reallocate if heap capacity is exhausted.
    /// Amortized O(1) complexity.
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

    /// Appends the contents of a string_view to the end of this string.
    /// If small and the result fits in SSO capacity, stays small.
    /// Otherwise, transitions to heap or grows existing heap allocation.
    /// May reallocate if capacity is insufficient.
    /// Complexity: O(sv.size()).
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

    /// Appends a single character to the end of this string.
    /// Delegates to push_back for consistent behavior.
    /// Amortized O(1) complexity.
    void append(char c) { push_back(c); }

    /// Appends the contents of a string_view to this string.
    /// Returns a reference to this string for chaining.
    /// Complexity: O(sv.size()).
    string& operator+=(string_view sv)
    {
        append(sv);
        return *this;
    }

    /// Appends a single character to this string.
    /// Returns a reference to this string for chaining.
    /// Amortized O(1) complexity.
    string& operator+=(char c)
    {
        append(c);
        return *this;
    }

    /// Concatenates a string and a string_view.
    /// Takes the left-hand string by value and appends the right-hand view.
    /// Returns a new string containing the concatenated result.
    /// Complexity: O(rhs.size()).
    [[nodiscard]] friend string operator+(string lhs, string_view rhs)
    {
        lhs.append(rhs);
        return lhs;
    }

    /// Concatenates a string and a character.
    /// Takes the left-hand string by value and appends the right-hand character.
    /// Returns a new string containing the concatenated result.
    /// Amortized O(1) complexity.
    [[nodiscard]] friend string operator+(string lhs, char rhs)
    {
        lhs.append(rhs);
        return lhs;
    }

    /// Clears the string to empty (size() becomes 0).
    /// Does not deallocate heap storage; capacity is preserved.
    /// After clear(), the string remains in its current mode (small or heap).
    /// Complexity: O(1).
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

    // comparisons
public:
    /// Compares this string with any type convertible to string_view for equality.
    /// Returns true if both have the same size and content.
    /// Complexity: O(size()).
    template <class S>
    [[nodiscard]] bool operator==(S&& rhs) const
        requires std::convertible_to<S, string_view>
    {
        return string_view(*this) == string_view(cc::forward<S>(rhs));
    }

    /// Compares this string with another string for equality.
    /// Returns true if both have the same size and content.
    /// Complexity: O(size()).
    [[nodiscard]] bool operator==(string const& rhs) const { return string_view(*this) == string_view(rhs); }

    /// Checks if this string starts with the given prefix.
    /// Returns true if the string begins with the prefix.
    /// Complexity: O(prefix.size()).
    [[nodiscard]] bool starts_with(string_view prefix) const { return string_view(*this).starts_with(prefix); }

    /// Checks if this string ends with the given suffix.
    /// Returns true if the string ends with the suffix.
    /// Complexity: O(suffix.size()).
    [[nodiscard]] bool ends_with(string_view suffix) const { return string_view(*this).ends_with(suffix); }

    /// Checks if this string contains the given substring.
    /// Returns true if the substring is found anywhere in the string.
    /// Complexity: O(size() * sv.size()).
    [[nodiscard]] bool contains(string_view sv) const { return string_view(*this).contains(sv); }

    /// Checks if this string contains the given character.
    /// Returns true if the character is found anywhere in the string.
    /// Complexity: O(size()).
    [[nodiscard]] bool contains(char c) const { return string_view(*this).contains(c); }

    // helpers
private:
    /// Checks if the string is currently in small (SSO) mode.
    /// Uses the low bit of the custom_resource pointer as a tag.
    /// Bit set (1) means small mode; bit clear (0) means heap mode.
    [[nodiscard]] bool is_small() const { return (reinterpret_cast<uintptr_t>(_data.small.custom_resource) & 1) != 0; }

    /// Returns the memory resource associated with this string.
    /// Removes the small-mode tag bit if present.
    /// nullptr means use the default global memory resource.
    [[nodiscard]] memory_resource const* resource() const { return remove_small_tag(_data.small.custom_resource); }

    /// Tags a memory resource pointer to indicate small mode.
    /// Sets the low bit to 1; the tagged pointer is stored in data_small.custom_resource.
    /// The low bit is safe to use because memory_resource pointers are always aligned.
    [[nodiscard]] static memory_resource const* add_small_tag(memory_resource const* r)
    {
        return reinterpret_cast<memory_resource const*>(reinterpret_cast<uintptr_t>(r) | 1);
    }

    /// Removes the small-mode tag from a tagged resource pointer.
    /// Clears the low bit to recover the original resource pointer (or nullptr).
    [[nodiscard]] static memory_resource const* remove_small_tag(memory_resource const* r)
    {
        return reinterpret_cast<memory_resource const*>(reinterpret_cast<uintptr_t>(r) & ~uintptr_t(1));
    }

    /// Initializes the union in small mode with size 0 and a tagged resource pointer.
    /// After this call, is_small() returns true and size() returns 0.
    void initialize_small_empty(memory_resource const* resource)
    {
        _data.small.size = 0;
        _data.small.custom_resource = add_small_tag(resource);
    }

    /// Initializes the union in heap mode by constructing data_heap and copying data.
    /// Allocates storage for len bytes, copies from str, and sets up the live range.
    /// Precondition: The union must be uninitialized or previously destroyed.
    void initialize_heap_from_data(char const* str, isize len, memory_resource const* resource);

    /// Transitions from small mode to heap mode.
    /// Copies the small buffer content to a new heap allocation with additional back capacity.
    /// Precondition: is_small() must be true.
    /// After this call, is_small() returns false and the old small data is lost.
    void materialize_heap(isize min_back_capacity);

    // data member
private:
    struct data_heap : cc::allocating_container<char, data_heap>
    {
        static constexpr bool uses_capacity_front = true;
        friend struct cc::string;
    };

    struct data_small
    {
        char data[small_capacity];
        u8 size;
        memory_resource const* custom_resource;
    };

    struct data_blocks
    {
        u64 blocks[6];
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
