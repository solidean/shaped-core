// The debuggee self-test.py traces. Its functions exist to be single-stepped, so they are built to
// survive the optimizer: self-test.py asserts on what they actually retire.
//
// __declspec(noinline) keeps the call and the stack frame; the volatile locals force real loads and
// stores so a body cannot fold to a constant or hoist out of the loop. Neither is an optimization
// artifact we are hoping for — both are guaranteed by the language and the attribute.
//
// extern "C" gives stable, unmangled names for --symbol. The shared `itrace_fixture_` prefix is also
// what makes the ambiguity test deterministic: any spec matching that prefix hits both functions.

#include <clean-core/string/print.hh>

extern "C" __declspec(noinline) int itrace_fixture_add(int a, int b)
{
    int volatile x = a;
    int volatile y = b;
    return x + y;
}

extern "C" __declspec(noinline) int itrace_fixture_mul(int a, int b)
{
    int volatile x = a;
    int volatile y = b;
    return x * y;
}

/// Calls one level deeper than main, so the entry stack has a frame worth asserting on.
__declspec(noinline) static int drive(int iterations)
{
    int accumulator = 0;
    for (int i = 0; i < iterations; ++i)
    {
        accumulator += itrace_fixture_add(i, 1);
        accumulator += itrace_fixture_mul(i, 2);
    }
    return accumulator;
}

int main()
{
    // 1000 iterations so --skip 100 always has a hit to land on.
    auto const result = drive(1000);

    // Consume the accumulator: without this the whole loop is dead code.
    cc::println("itrace-fixture: {}", result);
    return 0;
}
