#pragma once

#include <clean-core/string/interned_string.hh>
#include <clean-core/string/string_view.hh>
#include <typed-geometry/symbolic/fwd.hh>

// tg::symbol: a named variable of a computer-algebra polynomial, interned process-wide.

/// A variable by name: two symbols of the same name are the same variable, a pointer compare.
/// Variables are ordered by their names' bytes, so a polynomial's variable order, and with it its printed form, is the same every run.
struct tg::symbol
{
    symbol() = default;
    explicit symbol(cc::string_view name) : _name(cc::intern(name)) {}

    [[nodiscard]] cc::string_view name() const { return _name.as_string_view(); }

    [[nodiscard]] friend bool operator==(symbol a, symbol b) { return a._name == b._name; }
    /// By name, unsigned byte by byte.
    [[nodiscard]] friend bool operator<(symbol a, symbol b) { return a._name.compare_bytes(b._name) < 0; }

private:
    cc::interned_string _name;
};

/// A value for a variable, to evaluate or substitute by.
template <class X>
struct tg::assignment
{
    symbol variable;
    X value;
};
