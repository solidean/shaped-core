#include <clean-core/algorithm/sort.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/sequence/sequence.hh>
#include <clean-core/string/print.hh>
#include <nexus/test.hh>

#include <list>
#include <vector>

using namespace cc::primitive_defines;

EXAMPLE("clean-core/sequence-sources")
{
    // Sequences do not need a container: the factories make them from nothing.
    cc::println("squares below 50: {}",
                cc::iota(1, 8).map([](isize i) { return i * i; }).filter([](isize q) { return q < 50; }).count());
    cc::println("five sevens sum to {}", cc::repeat(7, 5).sum_from(0));

    // Some never end, which is part of their type: a reduction that would traverse them all does not compile.
    // take(n) is what turns one into something finite, and one that never ends is never empty, so first() needs no check.
    auto const powers = cc::iterate(1, [](int x) { return x * 2; }).take(10).to_vector();
    cc::println("2^9 = {}", powers.values().assert_non_empty().last());
    cc::println("first power above 1000: {}",
                cc::iterate(1, [](int x) { return x * 2; }).skip_while([](int x) { return x <= 1000; }).first());

    // single() is non-empty at compile time, so it seeds a chain that needs a first element.
    auto const readings = cc::vector<int>();
    cc::println("max with a floor of 0: {}", cc::single(0).chain(readings).max());

    // values_of turns any range into a sequence: std containers, C arrays, anything with begin and end.
    // Contiguous storage becomes a span, so it compiles to the loop you would have written.
    auto const legacy = std::vector<float>{2.5f, 4.0f, 1.5f};
    cc::println("legacy mean: {}", cc::values_of(legacy).assert_non_empty().average());

    auto const queue = std::list<int>{30, 10, 20};
    cc::println("list minimum: {}", cc::values_of(queue).assert_non_empty().min());

    // A C array of N > 0 elements is non-empty at compile time, like a fixed_array.
    int const histogram[] = {0, 3, 0, 5, 1};
    cc::println("busiest bucket: #{}", cc::indices_of(histogram).max_by([&](isize i) { return histogram[i]; }));

    // reversed() walks from the back, and last_index_of / find_last are the same searches from that end.
    auto const log = cc::vector<int>{200, 404, 200, 500, 200, 404};
    cc::println("last error at: #{}", log.values().last_index_of([](int code) { return code >= 400; }).value());
    cc::println("newest three codes, newest first:");
    log.values().reversed().take(3).each([](int code) { cc::println("  {}", code); });

    // Pointer reductions name an element rather than copy it, and the _into forms fold several sequences into one answer.
    auto const morning = cc::vector<int>{12, 9, 15};
    auto const evening = cc::vector<int>{8, 11};
    int const* coldest = nullptr;
    morning.values().min_ptr_into(coldest);
    evening.values().min_ptr_into(coldest);
    cc::println("coldest reading: {} (from the {} batch)", *coldest,
                coldest >= evening.data() && coldest < evening.data() + evening.size() ? "evening" : "morning");

    // Materializers end a chain in a container: sorted, grouped, fixed-size.
    auto const ranked = log.values().to_sorted_vector(cc::default_greater{});
    cc::println("worst code: {}", ranked[0]);
    auto const by_class = log.values().to_grouped_map([](int code) { return code / 100; });
    cc::println("2xx responses: {}, 4xx: {}, 5xx: {}", by_class.get(2).size(), by_class.get(4).size(),
                by_class.get(5).size());
}
