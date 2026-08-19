#pragma once

#include <clean-core/common/hash.hh> // derived containers define structural-hash hidden friends
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh> // the insertion family returns a span over the inserted elements
#include <clean-core/error/optional.hh>
#include <clean-core/memory/allocation.hh>

#include <initializer_list>
#include <new> // std::hardware_destructive_interference_size


/// Mixin implementing the common "contiguous container over cc::allocation<T>" surface area.
/// The shared container contracts it helps implement are in [containers](../../../../docs/containers.md).
///
/// This is a CRTP-style helper: concrete containers privately inherit it as
/// `cc::allocating_container<T, Derived>`, then selectively re-expose members via `using`.
/// Example (abridged):
///
///     template<class T>
///     struct cc::array : private cc::allocating_container<T, array<T>> {
///         using base = cc::allocating_container<T, array<T>>;
///         using base::operator[];
///         using base::data;
///         using base::begin; using base::end;
///         using base::size;  using base::empty;
///         // ... array-specific policies / ctors ...
///     };
///
/// The goal is to provide a shared, consistent foundation for any container whose storage and
/// object lifetime is modeled by `cc::allocation<T>` (i.e. a live object subrange inside an owned
/// byte allocation). This centralizes the "boring but sharp" parts: indexing, iteration, size
/// queries, and allocation-aware factories / extraction, while keeping policy decisions in the
/// actual container type.
///
/// Concrete containers can tailor:
/// - growth behavior: none like `array`, back-only like `vector`, or double-ended (no such container exists yet)
/// - invariants and allowed operations (push/pop/resize/rebalance)
/// - copy semantics (keep/delete/replace the provided deep-copy defaults)
///
/// Capacity is expressed directionally (`capacity_front` / `capacity_back`, plus the matching `has_capacity_*_for`) because a single `capacity()` is ambiguous across policies.
/// For a vector it typically means "append capacity", whereas a double-ended policy could mean a pooled budget or a total-possible size anchored at `obj_start`.
/// Containers are free to define their own `capacity()` (or `capacity_total()`) in terms of the directional primitives.
///
/// Member functions with the `_stable` suffix never reallocate the buffer and never move or invalidate live objects, so existing references, pointers and iterators stay valid.
/// They typically assert that enough allocation capacity is already present.
///
///
/// === Exception & reference guarantees ===
///
/// Applies to all push/emplace operations (except _stable variants, which never allocate).
///
/// Allocation failures leave the container unchanged.
/// Capacity may increase even if a subsequent operation fails.
/// Element construction failures leave size and live range unchanged.
///
/// Reallocation always uses move construction (no copy fallback).
/// If a move throws during reallocation, the container remains structurally valid
/// (size, bounds, iteration correct), but some elements may be in moved-from state.
///
/// The old allocation remains valid until new elements are constructed.
/// Constructing from existing elements (e.g. `emplace_back(v[i])`) is safe during growth.
///
/// Any reallocation invalidates pointers, references, and iterators.
///
/// Design favors predictable behavior over preserving values when moves throw.
///
/// The insertion family — emplace_at, insert_at, insert_range_at, replace_range — differs, and must not be read off the paragraphs above.
///
/// emplace_at and insert_at are STRONG against a throwing T(args...): the element is built into a temporary before anything moves, so a failure leaves size, live range and every value untouched.
///
/// insert_range_at and replace_range are BASIC.
/// If an element's copy or move constructor throws partway, the container is left holding the head plus the elements built so far — structurally valid, correctly sized and fully iterable.
/// The tail that was displaced to make room is destroyed rather than leaked.
/// The tail's VALUES are lost.
/// Buffering the whole range to do better is the caller's call, not ours.
///
/// Where an insertion reallocates it is strong against everything except a throwing move, since the entire result is built in the new buffer and the old one is only released at the very end.
template <class T, class ContainerT>
struct cc::allocating_container
{
    static_assert(std::is_object_v<T> && !std::is_const_v<T>,
                  "allocations need to refer to non-const objects, not references/functions/void");

    using container_t = ContainerT;

    /// Minimum alignment used for heap allocations of this container.
    ///
    /// We align allocations to at least one destructive-interference unit (typically a cache line).
    /// Combined with rounding allocation sizes to multiples of this value, this ensures that
    /// distinct container allocations never share a cache line, eliminating allocator-induced
    /// false sharing between containers.
    /// This removes "spooky" cross-object performance interference, while keeping alignment small enough to avoid the systematic cache-set aliasing larger alignments can cause.
    /// Larger-than-necessary alignment inside a single container — multiple elements per cache line, say — remains the programmer's responsibility by design.
    ///
    /// Deliberately a function, not a static constexpr variable: its body uses alignof(T), so as a data member its initializer would require a complete T.
    /// MSVC evaluates that initializer eagerly when the container specialization is instantiated, which breaks recursive or incomplete element types.
    /// A struct holding a cc::vector of itself is the case that breaks.
    /// As a function, alignof(T) is only evaluated on call, by which point T is complete.
    static constexpr isize alloc_alignment()
    {
        return cc::max(alignof(T), std::hardware_destructive_interference_size);
    }

    /// Maximum extra slack allowed when growing an allocation.
    ///
    /// We cap allocator leeway to one OS page (4 KiB) so allocators that naturally
    /// round to page granularity can return a full page, without letting small
    /// allocations balloon uncontrollably.
    static constexpr isize alloc_max_slack = 4096;

    /// Customization knob for deriving containers: if true, tries to preserve existing front capacity on some reallocation calls.
    ///
    /// True signals that the container actively uses front capacity, so a reallocation attempts to preserve existing front space while growing.
    /// False — `vector`'s setting — lets a back-only growth drop any existing front capacity rather than waste memory on unused space.
    /// No container sets it true today; it exists for a future double-ended policy.
    static constexpr bool uses_capacity_front = true;

    // element access
public:
    /// Returns a reference to the element at index i.
    /// Precondition: 0 <= i < size().
    [[nodiscard]] constexpr T& operator[](isize i)
    {
        auto const p_obj = _data.obj_start + i;
        CC_ASSERT(_data.obj_start <= p_obj && p_obj < _data.obj_end, "index out of bounds");
        return *p_obj;
    }
    [[nodiscard]] constexpr T const& operator[](isize i) const
    {
        auto const p_obj = _data.obj_start + i;
        CC_ASSERT(_data.obj_start <= p_obj && p_obj < _data.obj_end, "index out of bounds");
        return *p_obj;
    }

    /// Returns a reference to the first element.
    /// Precondition: !empty().
    [[nodiscard]] constexpr T& front()
    {
        CC_ASSERT(_data.obj_start < _data.obj_end, "allocation is empty");
        return *_data.obj_start;
    }
    [[nodiscard]] constexpr T const& front() const
    {
        CC_ASSERT(_data.obj_start < _data.obj_end, "allocation is empty");
        return *_data.obj_start;
    }

    /// Returns a reference to the last element.
    /// Precondition: !empty().
    [[nodiscard]] constexpr T& back()
    {
        CC_ASSERT(_data.obj_start < _data.obj_end, "allocation is empty");
        return *(_data.obj_end - 1);
    }
    [[nodiscard]] constexpr T const& back() const
    {
        CC_ASSERT(_data.obj_start < _data.obj_end, "allocation is empty");
        return *(_data.obj_end - 1);
    }

    /// Returns a pointer to the underlying contiguous storage.
    /// May be nullptr if the array is default-constructed or empty.
    [[nodiscard]] constexpr T* data() { return _data.obj_start; }
    [[nodiscard]] constexpr T const* data() const { return _data.obj_start; }

    // iterators
public:
    /// Returns a pointer to the first element.
    /// Enables range-based for loops.
    [[nodiscard]] constexpr T* begin() { return _data.obj_start; }
    /// Returns a pointer to one past the last element.
    [[nodiscard]] constexpr T* end() { return _data.obj_end; }
    [[nodiscard]] constexpr T const* begin() const { return _data.obj_start; }
    [[nodiscard]] constexpr T const* end() const { return _data.obj_end; }

    // queries
public:
    /// Returns the number of elements in the array.
    [[nodiscard]] constexpr isize size() const { return _data.obj_end - _data.obj_start; }
    /// Returns the total size in bytes of all elements in the array.
    [[nodiscard]] constexpr isize size_bytes() const { return (_data.obj_end - _data.obj_start) * sizeof(T); }
    /// Returns true if size() == 0.
    [[nodiscard]] constexpr bool empty() const { return _data.obj_start == _data.obj_end; }

    /// How many elements can be inserted at the front (via push_front) without reallocation.
    /// Computed as the number of whole sizeof(T) slots between alloc_start and obj_start,
    [[nodiscard]] constexpr isize capacity_front() const
    {
        // Note: pointer arithmetic is well-defined for the "all nullptr" case because the C++ standard
        //       explicitly defines nullptr - nullptr == 0 (no UB)
        auto const front_bytes = (cc::byte const*)_data.obj_start - _data.alloc_start;
        return front_bytes / sizeof(T); // floor
    }

    /// How many elements can be inserted at the back (via push_back) without reallocation.
    /// Computed as the number of whole sizeof(T) slots between obj_end and alloc_end.
    [[nodiscard]] constexpr isize capacity_back() const
    {
        // Note: pointer arithmetic is well-defined for the "all nullptr" case because the C++ standard
        //       explicitly defines nullptr - nullptr == 0 (no UB)
        auto const back_bytes = _data.alloc_end - (cc::byte const*)_data.obj_end;
        return back_bytes / sizeof(T); // floor
    }

    /// Cheap predicate: do we have room to grow the live range by `count` elements at the front
    /// without reallocation?
    [[nodiscard]] constexpr bool has_capacity_front_for(isize count) const
    {
        // Note: pointer arithmetic is well-defined for the "all nullptr" case because the C++ standard
        //       explicitly defines nullptr - nullptr == 0 (no UB)
        auto const front_bytes = (cc::byte const*)_data.obj_start - _data.alloc_start;
        return front_bytes >= count * isize(sizeof(T));
    }

    /// Cheap predicate: do we have room to grow the live range by `count` elements at the back
    /// without reallocation?
    [[nodiscard]] constexpr bool has_capacity_back_for(isize count) const
    {
        // Note: pointer arithmetic is well-defined for the "all nullptr" case because the C++ standard
        //       explicitly defines nullptr - nullptr == 0 (no UB)
        auto const back_bytes = _data.alloc_end - (cc::byte const*)_data.obj_end;
        return back_bytes >= count * isize(sizeof(T));
    }

    // resizing
public:
    // Computes the next allocation size when growing the container.
    //
    // Uses exponential growth (doubling) to ensure amortized constant-time growth,
    // then rounds up to the cache-line alignment used by this container.
    // This preserves allocator- and container-level guarantees against
    // cross-allocation false sharing while avoiding frequent small reallocations.
    [[nodiscard]] static constexpr isize alloc_grow_size_for(isize curr_size, isize min_size)
    {
        return cc::align_up(cc::max(curr_size << 1, min_size), alloc_alignment());
    }

private:
    // Helper: allocates a new buffer with the specified size and obj_offset,
    // moves existing objects to it, and replaces _data with the new allocation.
    void move_to_new_allocation(isize min_bytes, isize max_bytes, isize obj_offset)
    {
        CC_ASSERT((obj_offset + size()) * sizeof(T) <= min_bytes, "allocation too small for obj_offset + size");

        auto new_allocation = cc::allocation<T>::create_empty_bytes(min_bytes, max_bytes, alloc_alignment(),
                                                                    _data.custom_resource, obj_offset);

        // Move old elements to new allocation
        impl::move_create_objects_to(new_allocation.obj_end, _data.obj_start, _data.obj_end);

        // Replace current allocation
        _data = cc::move(new_allocation);
    }

    /// Ensures capacity to add count elements at the back, allocating if necessary.
    /// Returns a pointer to obj_end that can be used for construction and direct increment.
    ///
    /// Performance design:
    /// This function is marked CC_COLD_FUNC and should only be called from [[unlikely]] branches.
    /// The happy path (sufficient capacity) avoids function calls entirely, allowing the compiler
    /// to inline T(...) construction without any indirection.
    ///
    /// Usage pattern (begin/finalize sandwich):
    /// The returned pointer points to either &_data.obj_end (if realloc succeeded) or
    /// &new_allocation.obj_end (if full reallocation needed). This allows directly incrementing
    /// obj_end after each construction, writing through to the correct allocation.
    ///
    /// Example usage:
    ///   allocation<T> new_allocation;
    ///   auto p_obj_end = &_data.obj_end;
    ///
    ///   if (!has_capacity_back_for(1)) [[unlikely]]
    ///       p_obj_end = ensure_capacity_back_begin(new_allocation, 1);
    ///
    ///   // Construct new object into active alloc
    ///   // Do this BEFORE moving other objects because construction might reference them
    ///   // and so that a throwing T(...) doesn't break the container
    ///   // (an exception here means the empty new_allocation is cleaned up and container is untouched)
    ///   // NOTE: single syntactic construction site here (no branches) helps the inliner be more aggressive
    ///   auto const p = new (cc::placement_new, *p_obj_end) T(...);
    ///   (*p_obj_end)++; // _after_ so exceptions in T(...) leave state valid
    ///
    ///   if (new_allocation.is_valid()) [[unlikely]]
    ///       ensure_capacity_back_finalize(new_allocation);
    CC_COLD_FUNC [[nodiscard]] constexpr T** ensure_capacity_back_begin(allocation<T>& new_allocation, isize count)
    {
        CC_ASSERT(!has_capacity_back_for(count), "only call this if we don't have enough capacity");

        auto const new_capacity_front = container_t::uses_capacity_front ? capacity_front() : 0;
        auto const obj_size = size();

        // exponential growth strategy, at least sizeof(T) more
        auto const new_size_request_min = allocating_container::alloc_grow_size_for(
            (new_capacity_front + obj_size) * sizeof(T), (new_capacity_front + obj_size + count) * sizeof(T));
        auto const new_size_request_max = new_size_request_min + cc::min(new_size_request_min, alloc_max_slack);

        // try realloc first
        if (_data.try_resize_alloc_inplace(new_size_request_min, new_size_request_max))
            return &_data.obj_end;

        // otherwise we need a full new allocation
        // TODO: think about re-center logic if a double-ended container ever lands
        new_allocation = cc::allocation<T>::create_empty_bytes(new_size_request_min, new_size_request_max,
                                                               alloc_alignment(), _data.custom_resource);

        // Construct the new elements where they will sit in the new allocation, after the old ones.
        // The old allocation stays valid throughout the construction phase.
        // The new allocation's live range tracks only the newly-constructed elements, for exception safety:
        // if construction throws partway, only those need cleanup through new_allocation's dtor — the first k of a push_back_range that fails at k+1.
        // So the live range semantically starts behind the old allocation's live range, and finalize extends it to the full allocation once the old elements move over.
        new_allocation.obj_start += new_capacity_front + size();
        new_allocation.obj_end = new_allocation.obj_start;
        return &new_allocation.obj_end;
    }

    /// Finalizes the back capacity operation after elements have been constructed.
    /// PRECONDITION: new_allocation must be valid (i.e., full reallocation occurred, not just realloc).
    /// Moves old elements into the new allocation and replaces _data.
    /// The obj_end has already been updated via the pointer returned by ensure_capacity_back_begin.
    CC_COLD_FUNC constexpr void ensure_capacity_back_finalize(allocation<T>& new_allocation)
    {
        CC_ASSERT(new_allocation.is_valid(), "only call this when we have a temporary alloc");

        // Move over old elements in reverse order for exception safety with throwing move ctors
        // new_allocation.obj_start points to where the old elements should go (before newly constructed elements)
        // We move from _data in reverse: last element first, first element last
        // This is exception safe: if a move throws, new_allocation contains a valid contiguous range
        // (newly constructed elements at the end + successfully moved old elements in front)
        // Note: We don't decrement _data.obj_end during the move - moves in C++ are non-destructive
        // The old allocation remains in a moved-from but valid state, cleaned up when _data is destroyed below
        impl::move_create_objects_to_reverse(new_allocation.obj_start, _data.obj_start, _data.obj_end);

        // Replace the current allocation
        // This destroys _data (cleaning up the moved-from old elements) and adopts new_allocation
        _data = cc::move(new_allocation);
    }

    /// Scratch state threaded from _resize_gap_begin to _resize_gap_finalize.
    ///
    /// Its destructor is the unwind path.
    /// While a gap is open the container's live range covers only the head, and the tail sits above the gap alive but unowned.
    /// If the caller's element construction throws in between, this destroys that tail rather than leaking it.
    /// The container is then structurally valid, truncated to the head plus whatever was filled.
    /// On the reallocating path `new_allocation` cleans up the same way, and the container is left untouched.
    struct gap_scratch
    {
        cc::allocation<T> new_allocation; // valid only when begin had to build a fresh buffer
        T* displaced_start = nullptr;     // the in-place path's relocated tail, unowned until finalize re-adopts it
        T* displaced_end = nullptr;
        isize start = 0;
        isize old_count = 0;

        gap_scratch() = default;
        gap_scratch(gap_scratch const&) = delete;
        gap_scratch& operator=(gap_scratch const&) = delete;
        ~gap_scratch() { impl::destroy_objects_in_reverse(displaced_start, displaced_end); }
    };

    /// Opens a window of `new_count` RAW slots at index `start`, replacing the `old_count` live elements sitting there, and returns the cursor to construct through.
    /// The tail behind the replaced run moves exactly once, in whichever direction it needs to go, on both the in-place and the reallocating path.
    ///
    /// While the window is open the container's live range is deliberately only [obj_start, obj_start + start).
    /// The tail has already been relocated to its final position but is NOT owned by the container, so a throwing element construction truncates it instead of leaving raw slots inside a live range.
    /// `scratch`'s destructor destroys that tail on unwind, and _resize_gap_finalize re-adopts it on success.
    ///
    /// Preconditions: 0 <= start, 0 <= old_count, start + old_count <= size() and 0 <= new_count must hold.
    ///
    /// Usage pattern (begin/finalize sandwich, mirroring ensure_capacity_back_begin):
    ///   gap_scratch scratch;
    ///   auto p_obj_end = this->_resize_gap_begin(scratch, start, old_count, new_count);
    ///   auto* const gap = *p_obj_end;
    ///   for (...) { new (cc::placement_new, *p_obj_end) T(...); (*p_obj_end)++; }
    ///   this->_resize_gap_finalize(scratch);
    [[nodiscard]] constexpr T** _resize_gap_begin(gap_scratch& scratch, isize start, isize old_count, isize new_count)
    {
        CC_ASSERT(0 <= start && 0 <= old_count && start + old_count <= size(), "gap range out of bounds");
        CC_ASSERT(0 <= new_count, "gap size must be non-negative");

        scratch.start = start;
        scratch.old_count = old_count;

        auto const old_size = size();
        auto const tail_count = old_size - start - old_count;
        auto const delta = new_count - old_count;

        if (delta > 0 && !has_capacity_back_for(delta)) [[unlikely]]
        {
            // Same growth policy as ensure_capacity_back_begin, down to the slack and the in-place attempt.
            auto const new_capacity_front = container_t::uses_capacity_front ? capacity_front() : 0;
            auto const new_size_request_min = allocating_container::alloc_grow_size_for(
                (new_capacity_front + old_size) * sizeof(T), (new_capacity_front + old_size + delta) * sizeof(T));
            auto const new_size_request_max = new_size_request_min + cc::min(new_size_request_min, alloc_max_slack);

            if (!_data.try_resize_alloc_inplace(new_size_request_min, new_size_request_max))
            {
                scratch.new_allocation = cc::allocation<T>::create_empty_bytes(
                    new_size_request_min, new_size_request_max, alloc_alignment(), _data.custom_resource,
                    new_capacity_front);

                // Park the new allocation's live range exactly where the gap belongs, so it owns only the elements the caller is about to build.
                // A throw there destroys just those and leaves *this completely untouched, which is also what keeps the old elements readable for the whole construction phase.
                // Finalize then moves the head down in front of them and the tail up behind them, each landing directly in its final position.
                scratch.new_allocation.obj_start += start;
                scratch.new_allocation.obj_end = scratch.new_allocation.obj_start;
                return &scratch.new_allocation.obj_end;
            }
            // the in-place widening succeeded, so fall through with the room we now have
        }

        auto* const gap = _data.obj_start + start;
        impl::resize_object_window(gap, old_count, new_count, tail_count);

        // Hand the relocated tail to `scratch` and shrink the live range down to the head, so the open gap is never inside it.
        scratch.displaced_start = gap + new_count;
        scratch.displaced_end = scratch.displaced_start + tail_count;
        _data.obj_end = gap;

        return &_data.obj_end;
    }

    /// Closes the window opened by _resize_gap_begin and restores the container's live range.
    /// Precondition: the caller has constructed into every slot of the window and advanced the returned cursor past it.
    constexpr void _resize_gap_finalize(gap_scratch& scratch)
    {
        if (scratch.new_allocation.is_valid()) [[unlikely]]
        {
            auto* const head_start = _data.obj_start;
            auto* const head_end = _data.obj_start + scratch.start;
            auto* const tail_start = head_end + scratch.old_count;

            // Reverse for the head, so a throwing move still leaves new_allocation holding one contiguous live range.
            impl::move_create_objects_to_reverse(scratch.new_allocation.obj_start, head_start, head_end);
            impl::move_create_objects_to(scratch.new_allocation.obj_end, tail_start, _data.obj_end);

            // Adopting the new allocation destroys the old one, and with it the replaced elements — which is exactly why they stayed readable throughout the construction phase.
            _data = cc::move(scratch.new_allocation);
            return;
        }

        CC_ASSERT(_data.obj_end == scratch.displaced_start, "the gap was not filled completely");
        _data.obj_end = scratch.displaced_end;
        scratch.displaced_start = scratch.displaced_end = nullptr; // dismiss the unwind cleanup
    }

public:
    // destroys the live object range, so that obj_start == obj_end afterwards
    // calls all destructors, does not move obj_start
    constexpr void clear()
    {
        impl::destroy_objects_in_reverse(_data.obj_start, _data.obj_end);
        _data.obj_end = _data.obj_start;
    }

    /// Shrinks the container to the specified size by destroying trailing elements.
    /// Precondition: new_size <= size().
    /// Does not reallocate or change capacity.
    /// O(size() - new_size) complexity.
    constexpr void resize_down_to(isize new_size)
    {
        CC_ASSERT(new_size <= size(), "resize_down_to: new_size must be <= size()");

        auto const new_obj_end = _data.obj_start + new_size;
        impl::destroy_objects_in_reverse(new_obj_end, _data.obj_end);
        _data.obj_end = new_obj_end;
    }

    /// Resizes the container to the specified size, constructing new elements with the given arguments.
    /// If new_size <= size(), shrinks the container by destroying trailing elements.
    /// If new_size > size(), appends (new_size - size()) elements constructed with T(args...).
    /// May reallocate if growing beyond current capacity.
    /// Note: args are NOT forwarded - the same constructor arguments are reused for each element.
    /// Safe to use with arguments referencing the container (e.g., resize_to_constructed(10, v[0])).
    template <class... Args>
    void resize_to_constructed(isize new_size, Args&&... args)
    {
        static_assert(
            requires { T(args...); }, "resize_to_constructed: T is not constructible from the provided "
                                      "argument types");

        if (new_size <= size())
        {
            this->resize_down_to(new_size);
            return;
        }

        auto const count = new_size - size();

        // Use the ensure_capacity_back pattern to handle args potentially referencing old data
        allocation<T> new_allocation;
        auto p_obj_end = &_data.obj_end;

        if (!this->has_capacity_back_for(count)) [[unlikely]]
            p_obj_end = this->ensure_capacity_back_begin(new_allocation, count);

        // Construct new elements with args (not forwarded, so they can be reused)
        // This is safe even if args reference old data because old allocation remains valid
        for (isize i = 0; i < count; ++i)
        {
            new (cc::placement_new, *p_obj_end) T(args...);
            (*p_obj_end)++;
        }

        if (new_allocation.is_valid()) [[unlikely]]
            this->ensure_capacity_back_finalize(new_allocation);
    }

    /// Resizes the container to the specified size, filling new elements with copies of value.
    /// If new_size <= size(), shrinks the container by destroying trailing elements.
    /// If new_size > size(), appends (new_size - size()) copies of value.
    /// May reallocate if growing beyond current capacity.
    /// Safe to use with value references into the container (e.g., resize_to_filled(10, v[0])).
    void resize_to_filled(isize new_size, T const& value)
    {
        static_assert(std::is_copy_constructible_v<T>, "resize_to_filled requires T to be copy constructible");
        this->resize_to_constructed(new_size, value);
    }

    /// Resizes the container to the specified size, default-constructing new elements.
    /// If new_size <= size(), shrinks the container by destroying trailing elements.
    /// If new_size > size(), appends (new_size - size()) default-constructed elements.
    /// May reallocate if growing beyond current capacity.
    void resize_to_defaulted(isize new_size)
    {
        static_assert(std::is_default_constructible_v<T>, "resize_to_defaulted requires T to be default constructible");
        this->resize_to_constructed(new_size);
    }

    /// Clears the container and resizes it to new_size, constructing all elements with the given arguments.
    /// If new_size <= size(), shrinks the container by destroying trailing elements (no clear).
    /// If new_size > size(), destroys all existing elements, then constructs new_size elements with T(args...).
    /// May reallocate if growing beyond current capacity.
    /// Note: args are NOT forwarded - the same constructor arguments are reused for each element.
    /// Cannot use with arguments referencing the container (elements are cleared first).
    template <class... Args>
    void clear_resize_to_constructed(isize new_size, Args&&... args)
    {
        static_assert(
            requires { T(args...); }, "clear_resize_to_constructed: T is not constructible from the provided "
                                      "argument types");

        // Clear existing elements
        this->clear();

        // Reserve capacity for new elements
        this->reserve_back(new_size);

        // Construct new elements with args (not forwarded, so they can be reused)
        for (isize i = 0; i < new_size; ++i)
        {
            new (cc::placement_new, _data.obj_end) T(args...);
            _data.obj_end++;
        }
    }

    /// Clears the container and resizes it to new_size, default-constructing all elements.
    /// If new_size <= size(), shrinks the container by destroying trailing elements (no clear).
    /// If new_size > size(), destroys all existing elements, then constructs new_size default elements.
    /// May reallocate if growing beyond current capacity.
    void clear_resize_to_defaulted(isize new_size)
    {
        static_assert(std::is_default_constructible_v<T>, "clear_resize_to_defaulted requires T to be default "
                                                          "constructible");
        this->clear_resize_to_constructed(new_size);
    }

    /// Clears the container and resizes it to new_size, filling all elements with copies of value.
    /// If new_size <= size(), shrinks the container by destroying trailing elements (no clear).
    /// If new_size > size(), destroys all existing elements, then constructs new_size copies of value.
    /// May reallocate if growing beyond current capacity.
    /// Cannot use with value references into the container (elements are cleared first).
    void clear_resize_to_filled(isize new_size, T const& value)
    {
        static_assert(std::is_copy_constructible_v<T>, "clear_resize_to_filled requires T to be copy constructible");
        // TODO: could be split into a copy-assign prefix, then copy-constructed tail
        this->clear_resize_to_constructed(new_size, value);
    }

    /// Resizes the container to new_size with uninitialized memory.
    /// Only valid for trivially copyable and trivially destructible types.
    /// If new_size <= size(), shrinks the container.
    /// If new_size > size(), extends with uninitialized elements (only new elements are uninitialized).
    /// May reallocate if growing beyond current capacity - existing elements are preserved on reallocation.
    void resize_to_uninitialized(isize new_size)
    {
        static_assert(std::is_trivially_copyable_v<T>, "resize_to_uninitialized requires T to be trivially copyable");
        static_assert(std::is_trivially_destructible_v<T>, "resize_to_uninitialized requires T to be trivially "
                                                           "destructible");

        if (new_size <= size())
        {
            // Shrink: just adjust obj_end (trivially destructible, so no dtors needed)
            _data.obj_end = _data.obj_start + new_size;
            return;
        }

        // Grow: ensure capacity and extend obj_end
        auto const count = new_size - size();
        this->reserve_back(count);
        _data.obj_end = _data.obj_start + new_size;
    }

    /// Clears and resizes the container to new_size with uninitialized memory.
    /// Only valid for trivially copyable and trivially destructible types.
    /// If new_size <= size(), shrinks the container (no clear).
    /// If new_size > size(), clears existing elements and creates new_size uninitialized elements.
    /// May reallocate if growing beyond current capacity.
    /// On reallocation, existing content is not preserved (hence "clear").
    /// Optimizes allocation usage by repositioning obj_start/obj_end to maximize available space.
    void clear_resize_to_uninitialized(isize new_size)
    {
        static_assert(std::is_trivially_copyable_v<T>, "clear_resize_to_uninitialized requires T to be trivially "
                                                       "copyable");
        static_assert(std::is_trivially_destructible_v<T>, "clear_resize_to_uninitialized requires T to be trivially "
                                                           "destructible");

        if (new_size <= size())
        {
            // Shrink: just adjust obj_end (trivially destructible, so no dtors needed)
            _data.obj_end = _data.obj_start + new_size;
            return;
        }

        // Reposition obj_start/obj_end to first valid position (aligned for T)
        // This maximizes available space by starting from the beginning of the allocation
        auto const aligned_start = (T*)cc::align_up(_data.alloc_start, alignof(T));
        _data.obj_start = aligned_start;
        _data.obj_end = aligned_start;

        // Reserve capacity for new_size elements - might resize or realloc
        this->reserve_back(new_size);

        // Set obj_end to create new_size uninitialized elements
        _data.obj_end = _data.obj_start + new_size;
    }

    /// Ensures at least `count` elements can be inserted at the back without reallocation.
    /// Uses exponential growth strategy to amortize future reallocations.
    /// If sufficient capacity already exists, this is a no-op.
    void reserve_back(isize count)
    {
        if (this->has_capacity_back_for(count))
            return;

        // Compute new allocation size for in-place resize
        // Preserve existing front capacity by growing from current alloc size
        auto const inplace_size_min = allocating_container::alloc_grow_size_for(
            _data.alloc_size_bytes(), _data.alloc_size_bytes() + count * sizeof(T));
        auto const inplace_size_max = inplace_size_min + cc::min(inplace_size_min, alloc_max_slack);

        // Try to resize in place first
        if (_data.try_resize_alloc_inplace(inplace_size_min, inplace_size_max))
            return;

        // In-place resize failed, allocate new buffer
        // Now we can choose how much front capacity to preserve
        auto const new_capacity_front = container_t::uses_capacity_front ? this->capacity_front() : 0;
        auto const obj_size = this->size();

        auto const new_size_min = allocating_container::alloc_grow_size_for(
            (new_capacity_front + obj_size) * sizeof(T), (new_capacity_front + obj_size + count) * sizeof(T));
        auto const new_size_max = new_size_min + cc::min(new_size_min, alloc_max_slack);

        this->move_to_new_allocation(new_size_min, new_size_max, new_capacity_front);
    }

    /// Ensures at least `count` elements can be inserted at the back without reallocation.
    /// Guarantees capacity_back() >= count without exponential growth protection.
    /// Allocates exactly the needed space (rounded up to alignment).
    /// If sufficient capacity already exists, this is a no-op.
    void reserve_back_exact(isize count)
    {
        if (this->has_capacity_back_for(count))
            return;

        // Compute exact needed size for in-place resize
        // Preserve existing front capacity by growing from current alloc size
        auto const inplace_size = cc::align_up(_data.alloc_size_bytes() + count * sizeof(T), alloc_alignment());

        // Try to resize in place first
        if (_data.try_resize_alloc_inplace(inplace_size, inplace_size))
            return;

        // In-place resize failed, allocate new buffer
        // Now we can choose how much front capacity to preserve
        auto const new_capacity_front = container_t::uses_capacity_front ? this->capacity_front() : 0;
        auto const obj_size = this->size();

        auto const new_size = cc::align_up((new_capacity_front + obj_size + count) * sizeof(T), alloc_alignment());

        this->move_to_new_allocation(new_size, new_size, new_capacity_front);
    }

    /// Ensures at least `count` elements can be inserted at the front without reallocation.
    /// Uses exponential growth strategy to amortize future reallocations.
    /// If sufficient capacity already exists, this is a no-op.
    void reserve_front(isize count)
    {
        if (has_capacity_front_for(count))
            return;

        // Compute new allocation size with exponential growth
        auto const needed_bytes = _data.alloc_size_bytes() + count * sizeof(T);
        auto const new_size_min = allocating_container::alloc_grow_size_for(_data.alloc_size_bytes(), needed_bytes);
        auto const new_size_max = new_size_min + cc::min(new_size_min, alloc_max_slack);

        this->move_to_new_allocation(new_size_min, new_size_max, count);
    }

    /// Ensures at least `count` elements can be inserted at the front without reallocation.
    /// Guarantees capacity_front() >= count without exponential growth protection.
    /// Allocates exactly the needed space (rounded up to alignment).
    /// If sufficient capacity already exists, this is a no-op.
    void reserve_front_exact(isize count)
    {
        if (this->has_capacity_front_for(count))
            return;

        // Compute exact needed size, aligned
        auto const needed_bytes = _data.alloc_size_bytes() + count * sizeof(T);
        auto const new_size = cc::align_up(needed_bytes, alloc_alignment());

        this->move_to_new_allocation(new_size, new_size, count);
    }

    /// Reduces allocation size to fit the current number of elements.
    /// Reallocates only if the tight allocation size (aligned up to alloc_alignment()) differs from current size.
    /// Idempotent: calling multiple times has the same effect as calling once.
    /// If the container is empty or already tight, this is a no-op.
    void shrink_to_fit()
    {
        // Compute tight allocation size (align_up of size * sizeof(T) to alloc_alignment())
        auto const tight_size = cc::align_up(this->size() * sizeof(T), alloc_alignment());

        // Only reallocate if current allocation size differs from tight size
        if (_data.alloc_size_bytes() == tight_size)
            return;

        // Move to new tight allocation with no front capacity (obj_offset = 0)
        this->move_to_new_allocation(tight_size, tight_size, 0);
    }

    // appends
public:
    /// Constructs a new element at the back using existing capacity.
    /// Requires `has_capacity_back_for(1)` to be true; caller must ensure capacity in advance.
    /// No allocation occurs; pointers, references, and iterators remain valid (stable operation).
    /// Strong exception safety; O(1) complexity.
    /// Low-level primitive for performance-critical or reference-sensitive code.
    template <class... Args>
    constexpr T& emplace_back_stable(Args&&... args)
    {
        static_assert(
            requires { T(cc::forward<Args>(args)...); }, "emplace_back_stable: T is not constructible from "
                                                         "the provided argument types");
        CC_ASSERT(this->has_capacity_back_for(1), "not enough capacity for emplace_back_stable");
        auto const p = new (cc::placement_new, _data.obj_end) T(cc::forward<Args>(args)...);
        _data.obj_end++; // _after_ so exceptions in T(...) leave the state valid
        return *p;
    }

    /// Copy-constructs a new element at the back using existing capacity.
    /// Requires `has_capacity_back_for(1)` to be true; caller must ensure capacity in advance.
    /// No allocation occurs; pointers, references, and iterators remain valid (stable operation).
    /// Strong exception safety; O(1) complexity.
    /// Low-level primitive for performance-critical or reference-sensitive code.
    constexpr T& push_back_stable(T const& value) { return this->emplace_back_stable(value); }

    /// Move-constructs a new element at the back using existing capacity.
    /// Requires `has_capacity_back_for(1)` to be true; caller must ensure capacity in advance.
    /// No allocation occurs; pointers, references, and iterators remain valid (stable operation).
    /// Strong exception safety; O(1) complexity.
    /// Low-level primitive for performance-critical or reference-sensitive code.
    constexpr T& push_back_stable(T&& value) { return this->emplace_back_stable(cc::move(value)); }

    /// Constructs a new element at the front using existing capacity.
    /// Requires `has_capacity_front_for(1)` to be true; caller must ensure capacity in advance.
    /// No allocation occurs; pointers, references, and iterators remain valid (stable operation).
    /// Strong exception safety; O(1) complexity.
    /// Low-level primitive for performance-critical or reference-sensitive code.
    template <class... Args>
    constexpr T& emplace_front_stable(Args&&... args)
    {
        static_assert(
            requires { T(cc::forward<Args>(args)...); }, "emplace_front_stable: T is not constructible from "
                                                         "the provided argument types");
        CC_ASSERT(this->has_capacity_front_for(1), "not enough capacity for emplace_front_stable");
        auto const p = new (cc::placement_new, _data.obj_start - 1) T(cc::forward<Args>(args)...);
        _data.obj_start--; // _after_ so exceptions in T(...) leave the state valid
        return *p;
    }

    /// Copy-constructs a new element at the front using existing capacity.
    /// Requires `has_capacity_front_for(1)` to be true; caller must ensure capacity in advance.
    /// No allocation occurs; pointers, references, and iterators remain valid (stable operation).
    /// Strong exception safety; O(1) complexity.
    /// Low-level primitive for performance-critical or reference-sensitive code.
    constexpr T& push_front_stable(T const& value) { return this->emplace_front_stable(value); }

    /// Move-constructs a new element at the front using existing capacity.
    /// Requires `has_capacity_front_for(1)` to be true; caller must ensure capacity in advance.
    /// No allocation occurs; pointers, references, and iterators remain valid (stable operation).
    /// Strong exception safety; O(1) complexity.
    /// Low-level primitive for performance-critical or reference-sensitive code.
    constexpr T& push_front_stable(T&& value) { return this->emplace_front_stable(cc::move(value)); }

    /// Appends a new element to the back, allocating if necessary.
    /// If has_capacity_back_for(1) is true, no invalidation of any kind occurs.
    /// Otherwise, see "Exception & reference guarantees" section above.
    /// Amortized O(1) complexity.
    template <class... Args>
    constexpr T& emplace_back(Args&&... args)
    {
        static_assert(
            requires { T(cc::forward<Args>(args)...); }, "emplace_back: T is not constructible from "
                                                         "the provided argument types");

        allocation<T> new_allocation;
        auto p_obj_end = &_data.obj_end;

        if (!this->has_capacity_back_for(1)) [[unlikely]]
            p_obj_end = this->ensure_capacity_back_begin(new_allocation, 1);

        auto const p = new (cc::placement_new, *p_obj_end) T(cc::forward<Args>(args)...);
        (*p_obj_end)++; // _after_ so exceptions in T(...) leave state valid

        if (new_allocation.is_valid()) [[unlikely]]
            this->ensure_capacity_back_finalize(new_allocation);

        return *p;
    }

    /// Appends a copy of the element to the back.
    /// See emplace_back for guarantees and complexity.
    constexpr T& push_back(T const& value) { return this->emplace_back(value); }

    /// Appends an element to the back via move.
    /// See emplace_back for guarantees and complexity.
    constexpr T& push_back(T&& value) { return this->emplace_back(cc::move(value)); }

    /// Appends every element of `range` to the back, allocating at most once.
    /// A sized range (one exposing `.size()`) is reserved up front, so a large append is a single allocation; an unsized range falls back to per-element `emplace_back`.
    /// If capacity already covers the whole range, no invalidation of any kind occurs.
    /// Fast path: a contiguous range of exactly T with trivially-copyable T is appended in one `cc::memcpy`.
    /// Amortized O(range size).
    template <class Range>
    constexpr void push_back_range(Range const& range)
    {
        if constexpr (requires { isize(range.size()); })
        {
            auto const count = isize(range.size());
            this->reserve_back(count);

            // A contiguous range of exactly T (trivially copyable) is a bulk memory copy, no per-element ctor.
            if constexpr (std::is_trivially_copyable_v<T> && requires(Range const& r) {
                              r.data();
                              requires std::is_same_v<std::remove_cv_t<std::remove_pointer_t<decltype(r.data())>>, T>;
                          })
            {
                if (count > 0)
                    cc::memcpy(_data.obj_end, range.data(), size_t(count) * sizeof(T));
                _data.obj_end += count;
            }
            else
            {
                for (auto&& e : range)
                    this->emplace_back_stable(cc::forward<decltype(e)>(e));
            }
        }
        else
        {
            for (auto&& e : range)
                this->emplace_back(cc::forward<decltype(e)>(e));
        }
    }

    /// Appends every element of `range` to the back using existing capacity.
    /// Requires `has_capacity_back_for(<range size>)`; caller must reserve in advance.
    /// No allocation occurs; pointers, references, and iterators remain valid (stable operation). O(range size).
    /// Fast path: a contiguous range of exactly T with trivially-copyable T is appended in one `cc::memcpy`.
    /// Low-level primitive for performance-critical or reference-sensitive code.
    template <class Range>
    constexpr void push_back_range_stable(Range const& range)
    {
        if constexpr (std::is_trivially_copyable_v<T> && requires(Range const& r) {
                          isize(r.size());
                          r.data();
                          requires std::is_same_v<std::remove_cv_t<std::remove_pointer_t<decltype(r.data())>>, T>;
                      })
        {
            auto const count = isize(range.size());
            CC_ASSERT(this->has_capacity_back_for(count), "not enough capacity for push_back_range_stable");
            if (count > 0)
                cc::memcpy(_data.obj_end, range.data(), size_t(count) * sizeof(T));
            _data.obj_end += count;
        }
        else
        {
            for (auto&& e : range)
                this->emplace_back_stable(cc::forward<decltype(e)>(e));
        }
    }

    // TODO:
    // - emplace_front
    // - push_front
    // - push_front_range

    // insertions
public:
    /// Constructs a new element at index `idx` from `args...`, shifting everything at or after `idx` up by one, and returns a reference to it.
    ///
    /// The element is built into a temporary FIRST and only then moved into place.
    /// That is what makes `v.emplace_at(i, v[0])` safe and what keeps a throwing T(args...) from changing the container at all, unlike emplace_back, which can construct straight into its destination.
    /// `idx == size()` is the append position and is legal.
    /// Precondition: 0 <= idx <= size().
    /// Amortized O(size() - idx); allocates at most once.
    /// Invalidates pointers, references and iterators at or after `idx`, and all of them if the container grew.
    template <class... Args>
    constexpr T& emplace_at(isize idx, Args&&... args)
    {
        static_assert(
            requires { T(cc::forward<Args>(args)...); }, "emplace_at: T is not constructible from "
                                                         "the provided argument types");
        static_assert(std::is_move_constructible_v<T>, "emplace_at: T must be move constructible to make room for the "
                                                       "new element");
        CC_ASSERT(0 <= idx && idx <= size(), "emplace_at index out of bounds");

        // Built before anything moves, so args may reference this container's own elements and a throwing constructor leaves the container untouched.
        auto value = T(cc::forward<Args>(args)...);

        gap_scratch scratch;
        auto p_obj_end = this->_resize_gap_begin(scratch, idx, 0, 1);
        auto const p = new (cc::placement_new, *p_obj_end) T(cc::move(value));
        (*p_obj_end)++; // _after_ so exceptions in T(T&&) leave state valid
        this->_resize_gap_finalize(scratch);

        return *p;
    }

    /// Inserts a copy of `value` at index `idx`.
    /// See emplace_at for guarantees and complexity.
    constexpr T& insert_at(isize idx, T const& value) { return this->emplace_at(idx, value); }

    /// Inserts `value` at index `idx` by move.
    /// See emplace_at for guarantees and complexity.
    constexpr T& insert_at(isize idx, T&& value) { return this->emplace_at(idx, cc::move(value)); }

    /// Inserts the elements of `range` at index `idx`, and returns the span of the newly inserted elements.
    ///
    ///   [head][tail]  ->  [head][range][tail]        with `idx` elements in the head
    ///
    /// `range` must be sized, and must not alias this container's own elements — see replace_range, which this forwards to with an empty replaced run.
    /// `idx == size()` is the append position and is legal.
    /// Precondition: 0 <= idx <= size().
    /// Amortized O(size() - idx + range size); allocates at most once.
    /// Invalidates pointers, references and iterators at or after `idx`, and all of them if the container grew.
    template <class Range>
    constexpr cc::span<T> insert_range_at(isize idx, Range const& range)
    {
        CC_ASSERT(0 <= idx && idx <= size(), "insert_range_at index out of bounds");
        return this->replace_range({.offset = idx, .size = 0}, range);
    }

    /// Swaps the run of elements `r` out for the elements of `range`, and returns the span the range now occupies.
    ///
    ///   [head][r.size elements at r.offset][tail]  ->  [head][range][tail]
    ///
    /// `range` replaces the run as a whole, so `r.size` and `range.size()` are independent — the container grows or
    /// shrinks by the difference, and either may be zero.
    /// `r.size == 0` inserts at `r.offset` without removing anything, and an empty `range` removes the run.
    ///
    /// The tail behind the replaced run moves exactly ONCE, whether `range` is shorter, equal or longer than the run.
    /// That single-shuffle property is the whole reason this exists next to a remove_at_range followed by an insert_range_at, which would move the tail twice.
    ///
    /// `range` must be SIZED, i.e. expose `.size()`.
    /// Unlike push_back_range there is no per-element fallback for an unsized range, because the gap has to be opened before a single element can be read.
    /// `range` must also NOT alias this container's own elements: on the non-reallocating path the tail has already been shifted by the time the range is read.
    /// push_back_range carries no such restriction — copy the range first if it overlaps.
    /// Precondition: 0 <= r.offset && 0 <= r.size && r.offset + r.size <= size().
    /// Fast path: a contiguous range of exactly T with trivially-copyable T is copied into the gap in one `cc::memcpy`.
    /// Amortized O(size() - r.offset + range size); allocates at most once.
    /// Basic exception safety, and strong when it reallocates — see the guarantees at the top of this header.
    /// Invalidates pointers, references and iterators at or after `r.offset`, and all of them if the container grew.
    template <class Range>
    constexpr cc::span<T> replace_range(cc::offset_size r, Range const& range)
    {
        static_assert(
            requires(Range const& r2) { isize(r2.size()); },
            "replace_range / insert_range_at need a SIZED range (one exposing .size()): the gap has to be "
            "opened before any element can be read, so there is no per-element fallback the way "
            "push_back_range has one. Materialize the range into a cc::vector first.");
        CC_ASSERT(0 <= r.offset && 0 <= r.size && r.offset + r.size <= size(), "replace_range range out of bounds");

        auto const start = r.offset;
        auto const count = r.size;

        auto const new_count = isize(range.size());

        // Aliasing is only checkable for the case we can see: a contiguous range of exactly our element type.
        // It is worth asserting rather than only documenting, because the failure is capacity-dependent — it "works" whenever the container has to grow.
        if constexpr (requires(Range const& r) {
                          r.data();
                          requires std::is_same_v<std::remove_cv_t<std::remove_pointer_t<decltype(r.data())>>, T>;
                      })
        {
            auto const* const p_src = range.data();
            CC_ASSERT(new_count == 0 || p_src + new_count <= _data.obj_start || p_src >= _data.obj_end,
                      "replace_range source must not alias the container's own elements — copy it first");
        }

        gap_scratch scratch;
        auto p_obj_end = this->_resize_gap_begin(scratch, start, count, new_count);
        auto* const gap = *p_obj_end;

        // A contiguous range of exactly T (trivially copyable) is a bulk memory copy, no per-element ctor.
        if constexpr (std::is_trivially_copyable_v<T> && requires(Range const& r) {
                          r.data();
                          requires std::is_same_v<std::remove_cv_t<std::remove_pointer_t<decltype(r.data())>>, T>;
                      })
        {
            if (new_count > 0)
                cc::memcpy(gap, range.data(), size_t(new_count) * sizeof(T));
            *p_obj_end += new_count;
        }
        else
        {
            for (auto&& e : range)
            {
                new (cc::placement_new, *p_obj_end) T(cc::forward<decltype(e)>(e));
                (*p_obj_end)++; // _after_ so a throwing element ctor leaves state valid
            }
            CC_ASSERT(*p_obj_end == gap + new_count, "range.size() disagreed with the elements the range yielded");
        }

        this->_resize_gap_finalize(scratch);

        return cc::span<T>(gap, new_count);
    }

    // removals
public:
    /// Removes and returns the last element by move.
    /// Precondition: !empty().
    /// O(1) complexity.
    /// NOTE: Prefer remove_back() if you don't need the return value (avoids an extra move).
    [[nodiscard("use remove_back() if you don't need the return value")]] constexpr T pop_back()
    {
        CC_ASSERT(_data.obj_start < _data.obj_end, "cannot pop from empty container");
        auto value = cc::move(*(_data.obj_end - 1));
        (_data.obj_end - 1)->~T();
        _data.obj_end--;
        return value;
    }

    /// Removes the last element.
    /// Precondition: !empty().
    /// O(1) complexity.
    /// Fast path: destroys the element in place without moving.
    constexpr void remove_back()
    {
        CC_ASSERT(_data.obj_start < _data.obj_end, "cannot remove from empty container");
        _data.obj_end--;
        _data.obj_end->~T();
    }

    /// Removes and returns the first element by move.
    /// Precondition: !empty().
    /// O(1) complexity.
    /// NOTE: Prefer remove_front() if you don't need the return value (avoids an extra move).
    [[nodiscard("use remove_front() if you don't need the return value")]] constexpr T pop_front()
    {
        CC_ASSERT(_data.obj_start < _data.obj_end, "cannot pop from empty container");
        auto value = cc::move(*_data.obj_start);
        _data.obj_start->~T();
        _data.obj_start++;
        return value;
    }

    /// Removes the first element.
    /// Precondition: !empty().
    /// O(1) complexity.
    /// Fast path: destroys the element in place without moving.
    constexpr void remove_front()
    {
        CC_ASSERT(_data.obj_start < _data.obj_end, "cannot remove from empty container");
        _data.obj_start->~T();
        _data.obj_start++;
    }

    /// Removes and returns the element at the given index by move.
    /// Precondition: 0 <= idx < size().
    /// O(n) complexity due to element compaction.
    /// NOTE: Prefer remove_at() if you don't need the return value (avoids an extra move).
    [[nodiscard("use remove_at() if you don't need the return value")]] constexpr T pop_at(isize idx)
    {
        auto const p_obj = _data.obj_start + idx;
        CC_ASSERT(_data.obj_start <= p_obj && p_obj < _data.obj_end, "index out of bounds");

        // Move out the value before compacting
        auto value = cc::move(*p_obj);

        // Compact remaining elements backward (move-assigns into p_obj, which is now moved-from but alive)
        impl::compact_move_objects_backward(p_obj, p_obj + 1, _data.obj_end);

        // The last element is now in moved-from state; destroy it and shrink
        _data.obj_end--;
        _data.obj_end->~T();

        return value;
    }

    /// Removes the element at the given index.
    /// Precondition: 0 <= idx < size().
    /// O(n) complexity due to element compaction.
    /// Fast path: compacts elements without an extra move for the return value.
    constexpr void remove_at(isize idx)
    {
        auto const p_obj = _data.obj_start + idx;
        CC_ASSERT(_data.obj_start <= p_obj && p_obj < _data.obj_end, "index out of bounds");

        // Compact remaining elements backward (move-assigns over p_obj)
        impl::compact_move_objects_backward(p_obj, p_obj + 1, _data.obj_end);

        // The last element is now in moved-from state; destroy it and shrink
        _data.obj_end--;
        _data.obj_end->~T();
    }

    /// Removes and returns the element at the given index by swapping with the last element.
    /// Does not preserve relative order of elements (hence _unordered suffix).
    /// Precondition: 0 <= idx < size().
    /// O(1) complexity.
    /// All references remain valid except for the last element.
    /// Preferred over pop_at() when element order doesn't matter.
    /// NOTE: Prefer remove_at_unordered() if you don't need the return value (avoids an extra move).
    [[nodiscard("use remove_at_unordered() if you don't need the return value")]] constexpr T pop_at_unordered(isize idx)
    {
        auto const p_obj = _data.obj_start + idx;
        CC_ASSERT(_data.obj_start <= p_obj && p_obj < _data.obj_end, "index out of bounds");

        // Move out the value at idx
        auto value = cc::move(*p_obj);

        // Move the last element into the gap (unless we're removing the last element)
        _data.obj_end--;
        if (p_obj != _data.obj_end)
            *p_obj = cc::move(*_data.obj_end);

        // Destroy the last element (now either moved-from or the original at idx if it was last)
        _data.obj_end->~T();

        return value;
    }

    /// Removes the element at the given index by swapping with the last element.
    /// Does not preserve relative order of elements (hence _unordered suffix).
    /// Precondition: 0 <= idx < size().
    /// O(1) complexity.
    /// All references remain valid except for the last element.
    /// Preferred over remove_at() when element order doesn't matter.
    /// Fast path: avoids the extra move required by pop_at_unordered().
    constexpr void remove_at_unordered(isize idx)
    {
        auto const p_obj = _data.obj_start + idx;
        CC_ASSERT(_data.obj_start <= p_obj && p_obj < _data.obj_end, "index out of bounds");

        // Move the last element into the gap (unless we're removing the last element)
        _data.obj_end--;
        if (p_obj != _data.obj_end)
            *p_obj = cc::move(*_data.obj_end);

        // Destroy the last element (now either moved-from or the original at idx if it was last)
        _data.obj_end->~T();
    }

    /// Removes the `r.size` elements at `r.offset` by moving trailing elements into the gap.
    /// Does not preserve relative order of elements (hence _unordered suffix).
    /// Precondition: 0 <= r.offset && r.offset + r.size <= size().
    /// O(r.size) complexity.
    /// References and pointers to elements before r.offset remain valid.
    /// More efficient than ordered removal when element order doesn't matter.
    constexpr void remove_at_range_unordered(cc::offset_size r)
    {
        CC_ASSERT(0 <= r.offset && r.offset + r.size <= size(), "range out of bounds");

        auto const start = r.offset;
        auto const count = r.size;

        if (count == 0)
            return;

        auto const gap_start = _data.obj_start + start;

        // Move the last 'count' elements into the gap
        impl::compact_move_objects_backward(gap_start, _data.obj_end - count, _data.obj_end);

        // Resize down, destroying the now-unneeded trailing elements
        this->resize_down_to(size() - count);
    }

    /// Removes a range of elements [start, end) by moving trailing elements into the gap.
    /// Does not preserve relative order of elements (hence _unordered suffix).
    /// Precondition: 0 <= start && start <= end && end <= size().
    /// O(end - start) complexity.
    /// References and pointers to elements before start remain valid.
    /// More efficient than ordered removal when element order doesn't matter.
    constexpr void remove_from_to_unordered(isize start, isize end)
    {
        CC_ASSERT(0 <= start && start <= end && end <= size(), "range out of bounds");
        this->remove_at_range_unordered({.offset = start, .size = end - start});
    }

    /// Removes the `r.size` elements at `r.offset` while preserving relative order.
    /// Precondition: 0 <= r.offset && r.offset + r.size <= size().
    /// O(n) complexity due to element compaction.
    /// References and pointers to elements before r.offset remain valid.
    constexpr void remove_at_range(cc::offset_size r)
    {
        CC_ASSERT(0 <= r.offset && r.offset + r.size <= size(), "range out of bounds");

        auto const start = r.offset;
        auto const count = r.size;

        if (count == 0)
            return;

        auto const gap_start = _data.obj_start + start;
        auto const gap_end = gap_start + count;

        // Move all elements after the gap backward to close it
        impl::compact_move_objects_backward(gap_start, gap_end, _data.obj_end);

        // Resize down, destroying the now-unneeded trailing elements
        this->resize_down_to(size() - count);
    }

    /// Removes a range of elements [start, end) while preserving relative order.
    /// Precondition: 0 <= start && start <= end && end <= size().
    /// O(n) complexity due to element compaction.
    /// References and pointers to elements before start remain valid.
    constexpr void remove_from_to(isize start, isize end)
    {
        CC_ASSERT(0 <= start && start <= end && end <= size(), "range out of bounds");
        this->remove_at_range({.offset = start, .size = end - start});
    }

    /// Removes all elements for which the predicate returns true.
    /// Preserves the relative order of surviving elements.
    /// Returns the number of removed elements.
    /// Predicate is invoked as pred(element) or pred(idx, element) for each element.
    /// Single-pass O(n) algorithm with move-assignment for surviving elements.
    /// References and pointers to surviving elements may be invalidated.
    template <class Pred>
    constexpr isize remove_all_where(Pred&& pred)
    {
        static_assert(cc::is_invocable_r<bool, Pred, T&> || cc::is_invocable_r<bool, Pred, isize, T&>,
                      "remove_all_where: predicate must be invocable with T& or (isize, T&) and return bool");

        auto write_pos = _data.obj_start;
        auto read_pos = _data.obj_start;

        // Single pass: move survivors forward, skip elements to remove
        while (read_pos != _data.obj_end)
        {
            auto const idx = isize(read_pos - _data.obj_start);
            if (!cc::invoke_with_optional_idx(idx, pred, *read_pos))
            {
                // Element survives - move it to write position if needed
                if (write_pos != read_pos)
                    *write_pos = cc::move(*read_pos);
                ++write_pos;
            }
            ++read_pos;
        }

        // Calculate how many were removed
        auto const removed_count = _data.obj_end - write_pos;

        // Resize down, destroying trailing elements
        this->resize_down_to(write_pos - _data.obj_start);

        return removed_count;
    }

    /// Removes the first element for which the predicate returns true.
    /// Returns the index of the removed element, or cc::nullopt if no element matched.
    /// Predicate is invoked as pred(element) or pred(idx, element) for each element.
    /// Stops calling the predicate once a match is found.
    /// O(n) complexity.
    /// References and pointers to elements after the removed element are invalidated.
    template <class Pred>
    constexpr cc::optional<isize> remove_first_where(Pred&& pred)
    {
        static_assert(cc::is_invocable_r<bool, Pred, T&> || cc::is_invocable_r<bool, Pred, isize, T&>,
                      "remove_first_where: predicate must be invocable with T& or (isize, T&) and return bool");

        auto p = _data.obj_start;
        while (p != _data.obj_end)
        {
            auto const idx = isize(p - _data.obj_start);
            if (cc::invoke_with_optional_idx(idx, pred, *p))
            {
                // Found the element to remove - compact everything after it backward and remove the trailing element
                impl::compact_move_objects_backward(p, p + 1, _data.obj_end);
                this->remove_back();
                return idx;
            }
            ++p;
        }

        // No element matched
        return cc::nullopt;
    }

    /// Removes the last element for which the predicate returns true.
    /// Returns the index of the removed element, or cc::nullopt if no element matched.
    /// Predicate is invoked as pred(element) or pred(idx, element) for each element.
    /// Stops calling the predicate once a match is found (scanning backward).
    /// O(n) complexity.
    /// References and pointers to elements after the removed element are invalidated.
    template <class Pred>
    constexpr cc::optional<isize> remove_last_where(Pred&& pred)
    {
        static_assert(cc::is_invocable_r<bool, Pred, T&> || cc::is_invocable_r<bool, Pred, isize, T&>,
                      "remove_last_where: predicate must be invocable with T& or (isize, T&) and return bool");

        auto p = _data.obj_end;
        while (p != _data.obj_start)
        {
            --p;
            auto const idx = isize(p - _data.obj_start);
            if (cc::invoke_with_optional_idx(idx, pred, *p))
            {
                // Found the element to remove - compact everything after it backward and remove the trailing element
                impl::compact_move_objects_backward(p, p + 1, _data.obj_end);
                this->remove_back();
                return idx;
            }
        }

        // No element matched
        return cc::nullopt;
    }

    /// Removes all elements that compare equal to the given value.
    /// Preserves the relative order of surviving elements.
    /// Returns the number of removed elements.
    /// Single-pass O(n) algorithm with move-assignment for surviving elements.
    /// References and pointers to surviving elements may be invalidated.
    constexpr isize remove_all_value(T const& value)
    {
        static_assert(requires { bool(value == value); }, "remove_all_value: T must support operator==");
        return this->remove_all_where([&value](T const& elem) { return elem == value; });
    }

    /// Removes the first element that compares equal to the given value.
    /// Returns the index of the removed element, or cc::nullopt if no element matched.
    /// Stops searching once a match is found.
    /// O(n) complexity.
    /// References and pointers to elements after the removed element are invalidated.
    constexpr cc::optional<isize> remove_first_value(T const& value)
    {
        static_assert(requires { bool(value == value); }, "remove_first_value: T must support operator==");
        return this->remove_first_where([&value](T const& elem) { return elem == value; });
    }

    /// Removes the last element that compares equal to the given value.
    /// Returns the index of the removed element, or cc::nullopt if no element matched.
    /// Stops searching once a match is found (scanning backward).
    /// O(n) complexity.
    /// References and pointers to elements after the removed element are invalidated.
    constexpr cc::optional<isize> remove_last_value(T const& value)
    {
        static_assert(requires { bool(value == value); }, "remove_last_value: T must support operator==");
        return this->remove_last_where([&value](T const& elem) { return elem == value; });
    }

    /// Retains only elements for which the predicate returns true (removes elements where predicate returns false).
    /// Preserves the relative order of surviving elements.
    /// Returns the number of removed elements.
    /// Predicate is invoked as pred(element) or pred(idx, element) for each element.
    /// Single-pass O(n) algorithm with move-assignment for surviving elements.
    /// References and pointers to surviving elements may be invalidated.
    template <class Pred>
    constexpr isize retain_all_where(Pred&& pred)
    {
        static_assert(cc::is_invocable_r<bool, Pred, T&> || cc::is_invocable_r<bool, Pred, isize, T&>,
                      "retain_all_where: predicate must be invocable with T& or (isize, T&) and return bool");

        auto write_pos = _data.obj_start;
        auto read_pos = _data.obj_start;

        // Single pass: move survivors forward, skip elements to remove
        while (read_pos != _data.obj_end)
        {
            auto const idx = isize(read_pos - _data.obj_start);
            if (cc::invoke_with_optional_idx(idx, pred, *read_pos))
            {
                // Element survives - move it to write position if needed
                if (write_pos != read_pos)
                    *write_pos = cc::move(*read_pos);
                ++write_pos;
            }
            ++read_pos;
        }

        // Calculate how many were removed
        auto const removed_count = _data.obj_end - write_pos;

        // Resize down, destroying trailing elements
        this->resize_down_to(write_pos - _data.obj_start);

        return removed_count;
    }

    // TODO:
    // special for SoA use cases
    // - remove_all_where_zipped(pred, containers&...) -> isize count

    // other mutations
public:
    // copies `value` into every live object of this container
    constexpr void fill(T const& value)
    {
        for (auto p = _data.obj_start; p != _data.obj_end; ++p)
            *p = value;
    }

    // ctors / allocation
public:
    // array directly from a previous allocation
    // simply treats the live objects as the array
    [[nodiscard]] static container_t create_from_allocation(cc::allocation<T> data)
    {
        container_t c;
        c._data = cc::move(data);
        return c;
    }

    // creates an empty container with the specified memory resource (no allocation)
    // resource can be nullptr, which means the global default allocator will be used
    [[nodiscard]] static container_t create_with_resource(cc::memory_resource const* resource)
    {
        container_t c;
        c._data.custom_resource = resource;
        return c;
    }

    /// Initializes a new container_t with "size" many defaulted elements.
    /// Precondition: size >= 0.
    [[nodiscard]] static container_t create_defaulted(isize size, cc::memory_resource const* resource = nullptr)
    {
        CC_ASSERT(size >= 0, "container size must be non-negative");
        auto const byte_size = cc::align_up(size * sizeof(T), alloc_alignment());
        auto result = cc::allocation<T>::create_empty_bytes(byte_size, byte_size, alloc_alignment(), resource);
        impl::default_create_objects_to(result.obj_end, size);
        return container_t::create_from_allocation(cc::move(result));
    }

    /// Initializes a new container_t with "size" many elements, all copy-constructed from "value".
    /// Precondition: size >= 0.
    [[nodiscard]] static container_t create_filled(isize size, T const& value, cc::memory_resource const* resource = nullptr)
    {
        CC_ASSERT(size >= 0, "container size must be non-negative");
        auto const byte_size = cc::align_up(size * sizeof(T), alloc_alignment());
        auto result = cc::allocation<T>::create_empty_bytes(byte_size, byte_size, alloc_alignment(), resource);
        impl::fill_create_objects_to(result.obj_end, size, value);
        return container_t::create_from_allocation(cc::move(result));
    }

    /// Initializes a new container_t with "size" many uninitialized elements, which is only safe for trivial types.
    /// Precondition: size >= 0.
    [[nodiscard]] static container_t create_uninitialized(isize size, cc::memory_resource const* resource = nullptr)
    {
        CC_ASSERT(size >= 0, "container size must be non-negative");
        static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable for uninitialized allocation");
        static_assert(std::is_trivially_destructible_v<T>, "T must be trivially destructible for uninitialized "
                                                           "allocation");

        auto const byte_size = cc::align_up(size * sizeof(T), alloc_alignment());
        auto result = cc::allocation<T>::create_empty_bytes(byte_size, byte_size, alloc_alignment(), resource);
        result.obj_end = result.obj_start + size;
        return container_t::create_from_allocation(cc::move(result));
    }

    // creates a deep copy of the provided span
    [[nodiscard]] static container_t create_copy_of(cc::span<T const> source,
                                                    cc::memory_resource const* resource = nullptr)
    {
        auto const byte_size = cc::align_up(source.size() * sizeof(T), alloc_alignment());
        auto result = cc::allocation<T>::create_empty_bytes(byte_size, byte_size, alloc_alignment(), resource);
        impl::copy_create_objects_to(result.obj_end, source.data(), source.data() + source.size());
        return container_t::create_from_allocation(cc::move(result));
    }

    /// Initializes a new container_t with reserved capacity but no live objects.
    /// At least "capacity" elements can then be inserted without reallocation, and the actual capacity may be larger
    /// due to cache-line alignment (alloc_alignment()).
    /// Precondition: capacity >= 0.
    [[nodiscard]] static container_t create_with_capacity(isize capacity, cc::memory_resource const* resource = nullptr)
    {
        CC_ASSERT(capacity >= 0, "container capacity must be non-negative");
        auto const byte_size = cc::align_up(capacity * sizeof(T), alloc_alignment());
        return container_t::create_from_allocation(
            cc::allocation<T>::create_empty_bytes(byte_size, byte_size, alloc_alignment(), resource));
    }

    allocating_container() = default;
    allocating_container(std::initializer_list<T> init, cc::memory_resource const* resource = nullptr)
    {
        auto const byte_size = cc::align_up(init.size() * sizeof(T), alloc_alignment());
        _data = cc::allocation<T>::create_empty_bytes(byte_size, byte_size, alloc_alignment(), resource);
        cc::impl::copy_create_objects_to(_data.obj_end, init.begin(), init.end());
    }
    ~allocating_container() = default;

    // move semantics are already fine via cc::allocation
    allocating_container(allocating_container&&) = default;
    allocating_container& operator=(allocating_container&&) = default;

    // deep copy semantics
    // containers that use this mix-in can simply delete their copy ctor if they do not want it
    allocating_container(allocating_container const& rhs)
    {
        auto const byte_size = cc::align_up(rhs.size() * sizeof(T), alloc_alignment());
        _data = cc::allocation<T>::create_empty_bytes(byte_size, byte_size, alloc_alignment(), rhs._data.custom_resource);
        cc::impl::copy_create_objects_to(_data.obj_end, rhs._data.obj_start, rhs._data.obj_end);
    }
    allocating_container& operator=(allocating_container const& rhs)
    {
        if (this != &rhs)
        {
            auto const byte_size = cc::align_up(rhs.size() * sizeof(T), alloc_alignment());
            auto new_data = cc::allocation<T>::create_empty_bytes(byte_size, byte_size, alloc_alignment(),
                                                                  _data.custom_resource); // keep lhs resource
            cc::impl::copy_create_objects_to(new_data.obj_end, rhs._data.obj_start, rhs._data.obj_end);
            _data = cc::move(new_data);
        }
        return *this;
    }

    /// Extracts and returns the underlying allocation, leaving the allocating_container empty.
    /// The returned `cc::allocation<T>` owns the backing storage and live objects.
    /// The allocating_container retains its memory resource for future use.
    /// Enables zero-copy interop with other contiguous containers.
    /// Complexity: O(1).
    cc::allocation<T> extract_allocation() { return cc::move(_data); }

public:
    cc::allocation<T> _data; // the allocation backing this container, explicit modification is fine for power users
};
