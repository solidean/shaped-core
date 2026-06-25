#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <nexus/fuzz/fwd.hh>

#include <type_traits>
#include <typeindex> // std::type_index: runtime type identity for the type-erased box
#include <typeinfo>

namespace nx::fuzz
{
/// A single, type-erased, heap-boxed value flowing through a fuzz program.
///
/// Move-only: a value lives in exactly one slot and is never silently copied. get<T&>() returns a
/// reference *into* the box, so an operation taking `T&` mutates the stored value in place (this is
/// how mutating operations are modeled). A default-constructed value is "void" and not valid.
struct fuzz_value
{
    /// Boxes a copy/move of `value`. The stored type is the decayed type of V.
    template <class V>
    [[nodiscard]] static fuzz_value create(V value)
    {
        using T = std::decay_t<V>;
        static_assert(std::is_move_constructible_v<T>, "fuzz values must be movable");
        fuzz_value v;
        v._type = std::type_index(typeid(T));
        v._data = new T(cc::move(value));
        v._deleter = [](void* p) { delete static_cast<T*>(p); };
        return v;
    }

    fuzz_value() = default;

    fuzz_value(fuzz_value const&) = delete;
    fuzz_value& operator=(fuzz_value const&) = delete;

    fuzz_value(fuzz_value&& other) noexcept : _type(other._type), _data(other._data), _deleter(other._deleter)
    {
        other._data = nullptr;
        other._deleter = nullptr;
    }

    fuzz_value& operator=(fuzz_value&& other) noexcept
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

    ~fuzz_value() { reset(); }

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
        CC_ASSERT(is_valid(), "fuzz value is not valid");
        CC_ASSERT(is<plain_t>(), "fuzz value accessed at the wrong type");
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
} // namespace nx::fuzz
