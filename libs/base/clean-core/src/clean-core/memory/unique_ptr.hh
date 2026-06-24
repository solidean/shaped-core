#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/fwd.hh>
#include <clean-core/memory/node_allocation.hh>

namespace cc
{
template <class T, class... Args>
[[nodiscard]] unique_ptr<T> make_unique(Args&&... args);
} // namespace cc

/// Single-owner heap-allocated handle for exactly one T (move-only).
/// Construct via cc::make_unique<T>(...); clear by assigning nullptr.
/// Backed by cc::node_allocation, so it uses the node memory resource and frees wait-free.
/// Like a raw pointer, const-ness of the unique_ptr does not propagate to the pointee.
/// This is not a container: for owned buffers use cc::unique_array or cc::vector.
///
/// Differences from std::unique_ptr: no custom deleter, no allocator parameter,
/// no release, no T[] support, no polymorphic/upcast conversions.
template <class T>
struct cc::unique_ptr
{
    static_assert(!std::is_array_v<T>, "cc::unique_ptr does not support arrays; use cc::unique_array instead");
    static_assert(std::is_object_v<T> && !std::is_const_v<T>,
                  "unique_ptr must own a non-const object, not a reference/function/void");

    // properties
public:
    [[nodiscard]] bool is_valid() const { return _alloc.is_valid(); }
    explicit operator bool() const { return _alloc.is_valid(); }

    [[nodiscard]] T* get() const { return _alloc.ptr; }

    // smart pointer interface
public:
    [[nodiscard]] T& operator*() const { return *_alloc; }
    [[nodiscard]] T* operator->() const { return _alloc.operator->(); }

    // ctors/dtor
public:
    unique_ptr() = default;
    unique_ptr(std::nullptr_t) {}

    unique_ptr(unique_ptr&&) noexcept = default;
    unique_ptr& operator=(unique_ptr&&) noexcept = default;
    unique_ptr(unique_ptr const&) = delete;
    unique_ptr& operator=(unique_ptr const&) = delete;

    // destroy the managed object (if any) and become empty
    unique_ptr& operator=(std::nullptr_t)
    {
        _alloc = {};
        return *this;
    }

    ~unique_ptr() = default;

    // comparison
public:
    // C++20 synthesizes != and the reversed operand orders from these
    friend bool operator==(unique_ptr const& a, unique_ptr const& b) { return a.get() == b.get(); }
    friend bool operator==(unique_ptr const& a, T const* b) { return a.get() == b; }
    friend bool operator==(unique_ptr const& a, std::nullptr_t) { return a.get() == nullptr; }

    friend u64 hash(unique_ptr const& v) { return reinterpret_cast<u64>(v.get()); }

    // factory
public:
    template <class U, class... Args>
    friend unique_ptr<U> make_unique(Args&&... args);

private:
    explicit unique_ptr(cc::node_allocation<T> alloc) : _alloc(cc::move(alloc)) {}

    // members
private:
    cc::node_allocation<T> _alloc;
};

template <class T, class... Args>
[[nodiscard]] cc::unique_ptr<T> cc::make_unique(Args&&... args)
{
    return cc::unique_ptr<T>(
        cc::node_allocation<T>::create_from(cc::default_node_allocator(), cc::forward<Args>(args)...));
}
