#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>

#include <type_traits>
#include <typeindex> // std::type_index: runtime type identity for the type-erased box
#include <typeinfo>

namespace nx
{
/// A single, type-erased, heap-boxed value. The shared substrate for typed test-argument passing
/// (nx::invoke_tests / INVOCABLE_TEST) and the fuzz engine's value slots.
///
/// Move-only: a value lives in exactly one slot and is never silently copied. get<T&>() returns a
/// reference *into* the box, so a callee taking `T&` mutates the stored value in place. A
/// default-constructed value is "void" and not valid. The stored type is always the *decayed* type
/// of whatever was boxed, so matching is on decayed identity (`T` and `T const&` box the same type).
struct typed_value
{
    /// Boxes a copy/move of `value`. The stored type is the decayed type of V.
    template <class V>
    [[nodiscard]] static typed_value create(V value)
    {
        using T = std::decay_t<V>;
        static_assert(std::is_move_constructible_v<T>, "typed values must be movable");
        typed_value v;
        v._type = std::type_index(typeid(T));
        v._data = new T(cc::move(value));
        v._deleter = [](void* p) { delete static_cast<T*>(p); };
        return v;
    }

    typed_value() = default;

    typed_value(typed_value const&) = delete;
    typed_value& operator=(typed_value const&) = delete;

    typed_value(typed_value&& other) noexcept : _type(other._type), _data(other._data), _deleter(other._deleter)
    {
        other._data = nullptr;
        other._deleter = nullptr;
    }

    typed_value& operator=(typed_value&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            _type = other._type;
            _data = other._data;
            _deleter = other._deleter;
            other._data = nullptr;
            other._deleter = nullptr;
        }
        return *this;
    }

    ~typed_value() { reset(); }

    [[nodiscard]] bool is_valid() const { return _data != nullptr; }
    [[nodiscard]] std::type_index type() const { return _type; }
    [[nodiscard]] void* data() const { return _data; }

    /// True if the boxed value is (decayed) T.
    template <class T>
    [[nodiscard]] bool is() const
    {
        return is_valid() && _type == std::type_index(typeid(std::decay_t<T>));
    }

    /// Accesses the value as T. T may be a value (copy), a (const) reference into the box, etc.
    template <class T>
    [[nodiscard]] T get() const
    {
        static_assert(!std::is_rvalue_reference_v<T>, "rvalue references are not supported");
        static_assert(!std::is_pointer_v<std::decay_t<T>>, "storing raw pointers is not supported");
        using plain_t = std::decay_t<T>;
        CC_ASSERT(is_valid(), "typed value is not valid");
        CC_ASSERT(is<plain_t>(), "typed value accessed at the wrong type");
        return *static_cast<plain_t*>(_data);
    }

    [[nodiscard]] bool get_bool() const { return get<bool>(); }

private:
    void reset()
    {
        if (_data != nullptr && _deleter != nullptr)
            _deleter(_data);
        _data = nullptr;
        _deleter = nullptr;
    }

    std::type_index _type = std::type_index(typeid(void));
    void* _data = nullptr;
    void (*_deleter)(void*) = nullptr;
};
} // namespace nx
