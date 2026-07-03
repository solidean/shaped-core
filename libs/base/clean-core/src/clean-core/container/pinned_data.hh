#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/array.hh>
#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh>

#include <memory>
#include <type_traits>

/// Owning, shareable view over a contiguous sequence of T: a span<T> plus a type-erased
/// shared owner that keeps the backing memory alive for as long as any copy exists.
/// Mutable or immutable depending on T vs T const, just like a span.
/// Recognized as a contiguous container via .data()/.size(), so it passes anywhere a
/// span is expected; use .span() for the explicit view.
/// Copying shares ownership (refcount bump); it does not deep-copy the elements.
///
/// std::shared_ptr is used deliberately for now: clean-core has no shared-ownership pointer yet.
template <class T>
struct cc::pinned_data
{
    // construction
public:
    /// Default pinned_data is empty: data() == nullptr, size() == 0, no owner.
    pinned_data() = default;

    /// Converting constructor: pinned_data<U> -> pinned_data<T> when U* converts to T*
    /// (e.g. pinned_data<int> -> pinned_data<int const>). Shares the same owner.
    template <class U>
        requires(std::is_convertible_v<U*, T*>)
    pinned_data(pinned_data<U> const& other) : _span(other._span), _owner(other._owner)
    {
    }

    // element access
public:
    /// Returns a pointer to the underlying contiguous storage; nullptr if empty.
    [[nodiscard]] T* data() const { return _span.data(); }

    /// Returns a reference to the element at index i.
    /// Precondition: 0 <= i < size().
    [[nodiscard]] T& operator[](isize i) const { return _span[i]; }

    /// Returns a reference to the first element.
    /// Precondition: !empty().
    [[nodiscard]] T& front() const { return _span.front(); }

    /// Returns a reference to the last element.
    /// Precondition: !empty().
    [[nodiscard]] T& back() const { return _span.back(); }

    // iterators
public:
    /// Returns a pointer to the first element; nullptr if empty. Enables range-based for.
    [[nodiscard]] T* begin() const { return _span.begin(); }
    /// Returns a pointer to one past the last element.
    [[nodiscard]] T* end() const { return _span.end(); }

    // queries
public:
    /// Returns the number of elements.
    [[nodiscard]] isize size() const { return _span.size(); }
    /// Returns the total size of the elements in bytes.
    [[nodiscard]] isize size_bytes() const { return _span.size_bytes(); }
    /// Returns true if size() == 0.
    [[nodiscard]] bool empty() const { return _span.empty(); }
    /// Returns the non-owning span view of the pinned data.
    [[nodiscard]] cc::span<T> span() const { return _span; }

    // subdata
public:
    /// Returns the subdata [offset, size()), sharing this pinned_data's owner.
    /// Precondition: 0 <= offset <= size().
    [[nodiscard]] pinned_data subdata(isize offset) const { return create_from_pin(_span.subspan(offset), _owner); }
    /// Returns the subdata [r.offset, r.offset + r.size), sharing this pinned_data's owner.
    [[nodiscard]] pinned_data subdata(offset_size r) const { return create_from_pin(_span.subspan(r), _owner); }
    /// Returns the subdata [r.start, r.end), sharing this pinned_data's owner.
    [[nodiscard]] pinned_data subdata(start_end r) const { return create_from_pin(_span.subspan(r), _owner); }

    /// Like subdata(offset) but clamps the range into [0, size()] instead of asserting.
    [[nodiscard]] pinned_data subdata_clamped(isize offset) const
    {
        return create_from_pin(_span.subspan_clamped(offset), _owner);
    }
    /// Like subdata(offset_size) but clamps the range into [0, size()] instead of asserting.
    [[nodiscard]] pinned_data subdata_clamped(offset_size r) const
    {
        return create_from_pin(_span.subspan_clamped(r), _owner);
    }
    /// Like subdata(start_end) but clamps the range into [0, size()] instead of asserting.
    [[nodiscard]] pinned_data subdata_clamped(start_end r) const
    {
        return create_from_pin(_span.subspan_clamped(r), _owner);
    }

    // reinterpretation
public:
    /// Reinterprets the pinned elements as U, sharing this pinned_data's owner. See span::reinterpret_as
    /// for the constraints (U, T trivially copyable; sizeof(T) divisible by sizeof(U); const preserved).
    template <class U>
    [[nodiscard]] pinned_data<U> reinterpret_as() const
    {
        return pinned_data<U>::create_from_pin(_span.template reinterpret_as<U>(), _owner);
    }

    /// Like reinterpret_as but returns nullopt when the total byte size is not divisible by sizeof(U).
    template <class U>
    [[nodiscard]] cc::optional<pinned_data<U>> try_reinterpret_as() const
    {
        auto const view = _span.template try_reinterpret_as<U>();
        if (!view.has_value())
            return {};
        return pinned_data<U>::create_from_pin(view.value(), _owner);
    }

    /// Reinterprets the pinned elements as immutable raw bytes, sharing the owner.
    [[nodiscard]] pinned_data<cc::byte const> as_bytes() const
    {
        return pinned_data<cc::byte const>::create_from_pin(_span.as_bytes(), _owner);
    }
    /// Reinterprets the pinned elements as mutable raw bytes, sharing the owner; only for non-const T.
    [[nodiscard]] pinned_data<cc::byte> as_mutable_bytes() const
    {
        return pinned_data<cc::byte>::create_from_pin(_span.as_mutable_bytes(), _owner);
    }

    // factories
public:
    /// Wraps an existing span and its shared owner into a pinned_data.
    /// The owner must keep the memory referenced by data alive.
    [[nodiscard]] static pinned_data create_from_pin(cc::span<T> data, std::shared_ptr<void> owner)
    {
        return pinned_data(data, cc::move(owner));
    }

    /// Allocates and pins size default-constructed elements. Only valid for non-const T.
    [[nodiscard]] static pinned_data create_defaulted(isize size, cc::memory_resource const* resource = nullptr)
    {
        return create_owning(cc::array<T>::create_defaulted(static_cast<size_t>(size), resource));
    }

    /// Allocates and pins size elements, each copy-constructed from value. Only valid for non-const T.
    [[nodiscard]] static pinned_data create_filled(isize size, T const& value, cc::memory_resource const* resource = nullptr)
    {
        return create_owning(cc::array<T>::create_filled(static_cast<size_t>(size), value, resource));
    }

    /// Allocates and pins size uninitialized elements (only safe for trivial T). Only valid for non-const T.
    [[nodiscard]] static pinned_data create_uninitialized(isize size, cc::memory_resource const* resource = nullptr)
    {
        return create_owning(cc::array<T>::create_uninitialized(static_cast<size_t>(size), resource));
    }

    /// Allocates and pins a deep copy of source. Only valid for non-const T.
    [[nodiscard]] static pinned_data create_copy_of(cc::span<T const> source,
                                                    cc::memory_resource const* resource = nullptr)
    {
        return create_owning(cc::array<T>::create_copy_of(source, resource));
    }

    // members
private:
    pinned_data(cc::span<T> s, std::shared_ptr<void> owner) : _span(s), _owner(cc::move(owner)) {}

    /// Moves an owning array onto the heap, pins it, and views its storage.
    [[nodiscard]] static pinned_data create_owning(cc::array<T>&& buffer)
    {
        auto buf = std::make_shared<cc::array<T>>(cc::move(buffer));
        // Read data()/size() before moving buf: argument evaluation order is unsequenced.
        auto const view = cc::span<T>(buf->data(), buf->size());
        return pinned_data(view, std::shared_ptr<void>(cc::move(buf)));
    }

    template <class U>
    friend struct cc::pinned_data;

    cc::span<T> _span;
    std::shared_ptr<void> _owner;
};

namespace cc
{
/// Wraps a shared_ptr to a contiguous container into a pinned_data, without copying elements.
/// The pinned_data shares ownership with p; the element type (and constness) follows the container.
/// A null p yields an empty pinned_data.
template <class Container>
    requires requires(Container& c) {
        { c.data() };
        { c.size() } -> std::convertible_to<isize>;
    }
[[nodiscard]] auto as_pinned_data(std::shared_ptr<Container> p)
{
    using elem = std::remove_reference_t<decltype(*p->data())>;
    if (!p)
        return pinned_data<elem>{};
    auto const view = cc::span<elem>(p->data(), p->size());
    return pinned_data<elem>::create_from_pin(view, std::shared_ptr<void>(cc::move(p)));
}

/// Creates a pinned_data from any contiguous container, pinning its elements. Chooses the
/// cheapest safe strategy:
///  1. c is already a shared_ptr of a contiguous container -> wrap it (zero copies).
///  2. c is an owning rvalue (not a borrow range) -> move it into a shared_ptr (zero element copies).
///  3. otherwise (a borrow range, or an lvalue) -> allocate an owned deep copy of the elements.
template <class Container>
[[nodiscard]] auto make_pinned_data(Container&& c)
{
    if constexpr (requires { cc::as_pinned_data(cc::forward<Container>(c)); })
    {
        // case 1: already a shared_ptr of a contiguous container
        return cc::as_pinned_data(cc::forward<Container>(c));
    }
    else
    {
        using elem = std::remove_reference_t<decltype(*c.data())>;
        constexpr bool can_move
            = !std::is_lvalue_reference_v<Container> && !cc::enable_borrowed_range<std::remove_cvref_t<Container>>;

        if constexpr (can_move)
        {
            // case 2: owning rvalue -> move into a shared_ptr
            auto sp = std::make_shared<std::remove_cvref_t<Container>>(cc::move(c));
            return cc::as_pinned_data(cc::move(sp));
        }
        else
        {
            // case 3: borrow range or lvalue -> allocate an owned copy (buffer is non-const,
            // even when the resulting pinned_data views it as const)
            using value = std::remove_const_t<elem>;
            auto buf = std::make_shared<cc::array<value>>(cc::array<value>::create_copy_of(cc::span<value const>(c)));
            // Read data()/size() before moving buf: argument evaluation order is unsequenced.
            auto const view = cc::span<elem>(buf->data(), buf->size());
            return pinned_data<elem>::create_from_pin(view, std::shared_ptr<void>(cc::move(buf)));
        }
    }
}
} // namespace cc
