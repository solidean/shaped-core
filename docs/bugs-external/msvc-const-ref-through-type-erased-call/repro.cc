// MSVC 19.51 /O2: a reference returned through a type-erased call points at freed stack.
//
// Standalone.
// No shaped-core, no third-party headers, nothing but <cstdio>.
// Build and run it through run.py, or by hand:
//   cl /O2 /std:c++20 /EHsc repro.cc && repro.exe
//
// Exit code 0 = correct, 1 = miscompiled.

#include <cstdio>

namespace
{
// The type-erasing call wrapper, cut down to the two members that matter.
// This is the shape of cc::function_ref: a void const* to the callable, plus a thunk that casts it back.
// The return type is a REFERENCE, which is the part the bug needs.
struct int_ref_fn
{
    void const* obj;
    int const& (*thunk)(void const*);

    template <class F>
    int_ref_fn(F const& f)
      : obj(&f), thunk([](void const* o) -> int const& { return (*static_cast<F const*>(o))(); })
    {
    }

    int const& operator()() const { return thunk(obj); }
};

// Kept out of line and un-inlinable so the caller cannot see through it to the address.
// Without this the optimizer constant-folds the whole thing and the bug does not appear.
__declspec(noinline) int read_through(int_ref_fn const& f) { return f(); }
} // namespace

int main()
{
    // A const local with a known initializer.
    // It is odr-used (the lambda binds a reference to it), so it must have an address that outlives the call.
    int const value = 42;

    auto const lambda = [&value]() -> int const& { return value; };

    int_ref_fn const ref = lambda;
    int const observed = read_through(ref);

    std::printf("expected 42, observed %d\n", observed);

    if (observed == 42)
    {
        std::printf("OK: the reference survived the type-erased call\n");
        return 0;
    }

    std::printf("MISCOMPILED: the returned reference does not point at `value`\n");
    return 1;
}
