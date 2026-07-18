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

// A named global with a known extent, so the memory views have a heap-region symbol to resolve.
// volatile so every touch is a real load/store rather than a cached register.
extern "C" int volatile itrace_global_counter = 0;

// Touches a stack array (frame region) and the global (heap region), so --sections memory has both
// to classify and name. noinline keeps the frame; volatile keeps the accesses.
extern "C" __declspec(noinline) int itrace_fixture_touch(int n)
{
    int volatile buffer[8];
    for (int i = 0; i < 8; ++i)
        buffer[i] = n + i;

    itrace_global_counter += buffer[n & 7];
    return itrace_global_counter;
}

/// Calls one level deeper than main, so the entry stack has a frame worth asserting on.
__declspec(noinline) static int drive(int iterations)
{
    int accumulator = 0;
    for (int i = 0; i < iterations; ++i)
    {
        accumulator += itrace_fixture_add(i, 1);
        accumulator += itrace_fixture_mul(i, 2);
        accumulator += itrace_fixture_touch(i);
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
