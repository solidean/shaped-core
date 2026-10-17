#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/sequence/sequence.hh>
#include <clean-core/string/print.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
// The caller may hand in nothing, so the reductions that need a first element are not available directly.
void report(cc::span<float const> latencies_ms)
{
    // min() would not compile here: the sequence is not known to be non-empty.
    // The _or and try_ forms say what an empty input means, at the call site.
    cc::println("  fastest: {} ms", latencies_ms.values().min_or(0.0f));

    if (auto const slowest = latencies_ms.values().try_max(); slowest.has_value())
        cc::println("  slowest: {} ms", slowest.value());

    // try_as_non_empty is the branch: inside it, every first-element reduction is available.
    if (auto s = latencies_ms.values().try_as_non_empty(); s.has_value())
        cc::println("  mean:    {} ms", cc::move(s.value()).average());
    else
        cc::println("  (no samples)");
}
} // namespace

EXAMPLE("clean-core/sequence-non-empty")
{
    // Whether a sequence is non-empty is part of its type, and sum, min, max, average, first and last need it.
    // There is no silent "T{} is zero" fallback, which is what makes them work for types without a zero.
    auto const samples = cc::vector<float>{4.5f, 3.0f, 7.25f, 5.0f};

    // When you know the input is not empty, say so; it is checked, and costs nothing for a vector.
    auto const sum = samples.values().assert_non_empty().sum();
    cc::println("sum of samples: {}", sum);
    cc::println("first {} / last {}", samples.values().assert_non_empty().first(),
                samples.values().assert_non_empty().last());

    cc::println("report over the samples:");
    report(samples);
    cc::println("report over nothing:");
    report({});

    // A fixed_array of N > 0 elements is non-empty at compile time, so no check is needed at all.
    auto const corners = cc::fixed_array<int, 4>{3, 9, 1, 4};
    cc::println("largest corner: {}", corners.values().max());
    cc::println("index sum:      {}", corners.indices().sum());

    // A filter cannot know whether anything passes without evaluating, so try_as_non_empty pulls one element to find out.
    // It holds on to it, so nothing is evaluated twice.
    if (auto big = samples.values().filter([](float x) { return x > 5.0f; }).try_as_non_empty(); big.has_value())
        cc::println("smallest sample above 5: {}", cc::move(big.value()).min());

    // sum_from is the sum for any sequence: its start value is also the accumulator type.
    // Summing bytes in 64 bits is spelled by the start value, and sum_from(0) over floats would not compile at all.
    auto const bytes = cc::vector<u8>{200, 200, 200};
    cc::println("bytes summed as u64: {}", bytes.values().sum_from(u64(0)));
    cc::println("bytes summed by sum(): {}",
                bytes.values().assert_non_empty().sum()); // u8 + u8 is int, so this does not wrap either
}
