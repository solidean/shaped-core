#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/print.hh>
#include <nexus/test.hh>

namespace
{
void print_values(cc::string_view label, cc::span<int const> values)
{
    cc::print("{:<22}", label);
    for (auto const v : values)
        cc::print(" {}", v);
    cc::println();
}
} // namespace

EXAMPLE("clean-core/vector")
{
    // The factories say what the elements ARE, so there is no "resize then overwrite" step.
    auto v = cc::vector<int>::create_filled(5, 1);
    print_values("create_filled(5, 1)", v);

    v.push_back(9);
    v.push_back_range(cc::vector<int>::create_filled(2, 7));
    print_values("after two appends", v);

    // Removal comes in an ordered and an unordered flavour, and the name says which you get.
    v.remove_at(0);
    v.remove_at_unordered(0);
    print_values("remove_at + unordered", v);

    // Predicate removal returns how many went, so a caller never has to compare sizes itself.
    auto const removed = v.remove_all_where([](int x) { return x == 7; });
    cc::println("removed {} sevens", removed);
    print_values("after remove_all_where", v);

    // A vector converts to a span for free, which is how a function takes "some ints" without taking ownership.
    print_values("as a span", cc::span<int const>(v));

    // pop_back hands the element back; remove_back drops it.
    // Two names rather than one nodiscard, because discarding is a perfectly good thing to want.
    auto const last = v.pop_back();
    cc::println("popped {}", last);

    v.clear();
    cc::println("cleared: size {}, capacity {}", v.size(), v.capacity());
}
