#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/node_allocation.hh>

/// Move-only owning callable wrapper with signature T
/// Similar to std::function but move-only, allowing capture of unique resources
/// Is actually even stronger: allows non-moveable "pinned" captures as well
/// Uses a cc::node_memory_resource under the hood and is therefore extremely efficient
/// Like unique_ptr<int>, const-ness of this object does not imply constness of the pointed-to object!
template <class R, class... Args>
struct cc::unique_function<R(Args...)>
{
public:
    R operator()(Args... args) const
    {
        CC_ASSERT(is_valid(), "cannot call in invalid cc::unique_function");
        return _thunk(_payload.ptr, cc::forward<Args>(args)...);
    }

    bool is_valid() const { return _payload.is_valid(); }
    explicit operator bool() const { return _payload.is_valid(); }

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

        // NOLINTBEGIN
        _payload = cc::node_allocation<Fn>::create_from(cc::default_node_allocator(), cc::forward<F>(f));
        _thunk = [](void* p, Args... args) -> R { return cc::invoke(*static_cast<Fn*>(p), cc::forward<Args>(args)...); };
        // NOLINTEND
    }

    // takes an explicit allocator
    // directly emplaces in target storage
    template <class F, class... FArgs>
    [[nodiscard]] static unique_function create_from(cc::node_allocator& alloc, FArgs&&... args)
    {
        unique_function uf;
        uf._payload = cc::node_allocation<F>::create_from(alloc, cc::forward<FArgs>(args)...);
        uf._thunk
            = [](void* p, Args... args) -> R { return cc::invoke(*static_cast<F*>(p), cc::forward<Args>(args)...); };
        return uf;
    }

    unique_function(unique_function&&) = default;
    unique_function(unique_function const&) = delete;
    unique_function& operator=(unique_function&&) = default;
    unique_function& operator=(unique_function const&) = delete;

    ~unique_function() = default;

    // member
private:
    // future: we could do SBO here in some cases
    cc::any_node_allocation _payload;
    cc::function_ptr<R(void*, Args...)> _thunk = nullptr;
};
