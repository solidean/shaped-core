#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/memory/node_allocation.hh>

/// Move-only owning callable wrapper with signature T
/// Similar to std::function but move-only, allowing capture of unique resources
/// Is actually even stronger: allows non-moveable "pinned" captures as well
/// Uses a cc::node_memory_resource under the hood and is therefore extremely efficient
/// Like unique_ptr<int>, const-ness of this object does not imply constness of the pointed-to object!
///
/// Layout: a SINGLE pointer (8 B). The closure and its type-erased ops live together in one node — a header
/// carrying a pointer to a static per-closure vtable (a plain data member, NOT a C++ vptr), followed by the
/// closure. cc::poly_node_allocation reads that header to destroy + free without knowing the closure type, so
/// the handle stays one pointer. (Previously an any_node_allocation + a separate thunk = 32 B.)

namespace cc::impl
{
template <class R, class... Args>
struct unique_function_node;

// Hand-rolled vtable for a type-erased callable node. Parameterized by the call signature only (not the
// closure), so one node_base type serves every closure with that signature.
template <class R, class... Args>
struct unique_function_vtable
{
    R (*invoke)(unique_function_node<R, Args...>* self, Args...);
    cc::node_class_index (*destroy)(unique_function_node<R, Args...>* self); // dtor the node, return its class index
};

// Header of every unique_function node; the concrete node derives and appends the closure.
template <class R, class... Args>
struct unique_function_node
{
    unique_function_vtable<R, Args...> const* vt;
};

template <class F, class R, class... Args>
struct unique_function_impl_node : unique_function_node<R, Args...>
{
    F closure;

    template <class... FArgs>
    explicit unique_function_impl_node(unique_function_vtable<R, Args...> const* vt, FArgs&&... args)
      : unique_function_node<R, Args...>{vt}, closure(cc::forward<FArgs>(args)...)
    {
    }
};

template <class F, class R, class... Args>
R unique_function_invoke(unique_function_node<R, Args...>* self, Args... args)
{
    auto* n = static_cast<unique_function_impl_node<F, R, Args...>*>(self);
    return cc::invoke(n->closure, cc::forward<Args>(args)...);
}

template <class F, class R, class... Args>
cc::node_class_index unique_function_destroy(unique_function_node<R, Args...>* self)
{
    using node_t = unique_function_impl_node<F, R, Args...>;
    constexpr auto idx = cc::node_class_index_for<node_t>();
    static_cast<node_t*>(self)->~node_t();
    return idx;
}

// One static vtable instance per (closure, signature). Its address is stable and stored in the node header.
template <class F, class R, class... Args>
inline constexpr unique_function_vtable<R, Args...> unique_function_vtable_for = {
    &unique_function_invoke<F, R, Args...>,
    &unique_function_destroy<F, R, Args...>,
};

// poly_node_allocation traits: recover the class index from the header vtable, destroying the node en route.
template <class R, class... Args>
struct unique_function_node_traits
{
    static cc::node_class_index destroy_and_get_class_index(unique_function_node<R, Args...>& b)
    {
        return b.vt->destroy(&b);
    }
};
} // namespace cc::impl

template <class R, class... Args>
struct cc::unique_function<R(Args...)>
{
public:
    R operator()(Args... args) const
    {
        CC_ASSERT(is_valid(), "cannot call in invalid cc::unique_function");
        return _node.ptr->vt->invoke(_node.ptr, cc::forward<Args>(args)...);
    }

    bool is_valid() const { return _node.is_valid(); }
    explicit operator bool() const { return _node.is_valid(); }

public:
    unique_function() = default;

    // note: this ctor requires moveability
    //       use the factory method for in-place construction or custom alloc
    //       always uses the system node resource
    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, unique_function>)
    unique_function(F&& f)
    {
        using Fn = std::remove_cvref_t<F>;

        // Future: check if we want this as requires or not
        static_assert(cc::is_invocable_r<R, Fn&, Args...>, "F must be callable with Args... and return R");
        static_assert(std::is_constructible_v<Fn, F>, "cannot copy/move into the unique_function. try direct "
                                                      "construction via create_from.");

        *this = create_from<Fn>(cc::default_node_allocator(), cc::forward<F>(f));
    }

    // takes an explicit allocator
    // directly emplaces in target storage
    template <class F, class... FArgs>
    [[nodiscard]] static unique_function create_from(cc::node_allocator& alloc, FArgs&&... args)
    {
        using node_t = cc::impl::unique_function_impl_node<F, R, Args...>;
        auto* const mem = alloc.allocate_node_bytes(cc::node_class_index_for<node_t>(), sizeof(node_t), alignof(node_t));
        auto* const n = new (cc::placement_new, mem)
            node_t(&cc::impl::unique_function_vtable_for<F, R, Args...>, cc::forward<FArgs>(args)...);

        unique_function uf;
        uf._node.ptr = n; // poly_node_allocation adopts the freshly built node
        return uf;
    }

    unique_function(unique_function&&) = default;
    unique_function(unique_function const&) = delete;
    unique_function& operator=(unique_function&&) = default;
    unique_function& operator=(unique_function const&) = delete;

    ~unique_function() = default;

    // member
private:
    cc::poly_node_allocation<cc::impl::unique_function_node<R, Args...>, cc::impl::unique_function_node_traits<R, Args...>> _node;
};
