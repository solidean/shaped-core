#include <clean-core/math/random.hh>
#include <nexus/fuzz/fuzz.hh>
#include <nexus/test.hh>

#include <string_view>

namespace
{
cc::unique_ptr<nx::fuzz::test> make_add1_test()
{
    auto t = nx::fuzz::test::create();
    t->add_value("3", 3);
    t->add_op("add1", [](int a) { return a + 1; });
    t->add_invariant("is-not-7", [](int i) { return i != 7; });
    return t;
}

std::string_view as_sv(cc::string const& s)
{
    return std::string_view(s.data(), size_t(s.size()));
}
} // namespace

TEST("fuzz regression - emitted code chained via eval_op reproduces the failure")
{
    // This mirrors what the emitter prints: rebuild the values via eval_op, chaining results.
    auto t = make_add1_test();

    auto i0 = t->eval_op("3");
    auto i1 = t->eval_op("add1", i0);
    auto i2 = t->eval_op("add1", i1);
    auto i3 = t->eval_op("add1", i2);
    auto i4 = t->eval_op("add1", i3);

    CHECK(i4.get<int>() == 7);
    CHECK(!t->eval_op_bool("is-not-7", i4)); // the invariant is false at 7 -> reproduces the finding
}

TEST("fuzz regression - emit_regression renders a usable reproducer")
{
    auto t = make_add1_test();

    nx::fuzz::test::fuzz_result res;
    for (int s = 1; s <= 64; ++s)
    {
        res = t->execute_fuzzer(s);
        if (!res.is_ok && res.failing_run.has_value())
            break;
    }
    REQUIRE(res.failing_run.has_value());

    cc::random rng{1u};
    auto minimized = res.failing_run.value().minimize(rng);
    auto code = minimized.emit_regression("test", nx::fuzz::nexus_section_dialect());
    auto sv = as_sv(code);

    CHECK(sv.find("eval_op") != std::string_view::npos);
    CHECK(sv.find("add1") != std::string_view::npos);
    CHECK(sv.find("is-not-7") != std::string_view::npos);
    CHECK(sv.find("eval_op_bool") != std::string_view::npos);
}

TEST("fuzz regression - replay_random reproduces a seeded operation")
{
    auto t = nx::fuzz::test::create();
    t->add_op("gen", [](cc::random& r) { return r.uniform(0, 1000000); });

    nx::fuzz::replay_random random;
    // same seed -> same drawn value, every time
    int const a = t->eval_op_to<int>("gen", random.seeded(4242));
    int const b = t->eval_op_to<int>("gen", random.seeded(4242));
    CHECK(a == b);
}
