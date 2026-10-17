#include <clean-core/container/vector.hh>
#include <clean-core/sequence/sequence.hh>
#include <clean-core/string/print.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

EXAMPLE("clean-core/sequence-basics")
{
    auto temperatures = cc::vector<int>{12, 17, 9, 21, 25, 18, 7};

    // values() is the container as a lazy sequence.
    // It references the vector, so it is used in one expression and never stored, like a span.
    cc::println("days:          {}", temperatures.values().count());
    cc::println("warm days:     {}", temperatures.values().count_if([](int t) { return t >= 18; }));
    cc::println("any frost:     {}", temperatures.values().any([](int t) { return t <= 0; }));
    cc::println("all above 5:   {}", temperatures.values().all([](int t) { return t > 5; }));
    cc::println("ever exactly 21: {}", temperatures.values().contains(21));

    // index_of answers where, as an optional, so a miss is not a magic -1.
    if (auto const day = temperatures.values().index_of([](int t) { return t > 20; }); day.has_value())
        cc::println("first day above 20: #{}", day.value());

    // find answers which, as a pointer into the vector, so the element can be changed in place.
    if (int* const first_warm = temperatures.values().find([](int t) { return t >= 18; }))
        *first_warm += 1;

    // Any callback may take the position as a leading isize.
    temperatures.values().each([](isize day, int t) { cc::println("  day {}: {} C", day, t); });

    // A sum over a sequence that might be empty starts from a value, and that value's type is the accumulator.
    cc::println("total degrees: {}", temperatures.values().sum_from(0));

    // indices() is the index sequence of an indexed container; its contains() is a bounds check, not a scan.
    cc::println("day 6 recorded: {}", temperatures.indices().contains(6));
    cc::println("day 7 recorded: {}", temperatures.indices().contains(7));

    // map and filter are lazy: nothing runs until a terminal operation such as to_vector pulls the elements through.
    auto const fahrenheit_of_warm_days
        = temperatures.values().filter([](int t) { return t >= 18; }).map([](int t) { return t * 9 / 5 + 32; }).to_vector();
    cc::println("warm days in F:");
    for (int f : fahrenheit_of_warm_days)
        cc::println("  {}", f);

    // A sequence is single-pass, and a terminal operation consumes it.
    // That is why a named one is spent with cc::move, which keeps the second pass from compiling by accident.
    auto week = temperatures.values().take(5);
    cc::println("weekday total: {}", cc::move(week).sum_from(0));

    // Range-for works on every sequence.
    // Over values() itself it is a plain pointer loop, so there is no cost to reaching for it.
    for (int& t : temperatures.values())
        t -= 1;
    cc::println("after calibration, day 0: {} C", temperatures[0]);
}
