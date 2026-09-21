#include "random-program.hh"

using namespace sgl_test;
using namespace sgl::check;

namespace
{
/// Small programs first: a seed that fails at a small shape is the one worth reading.
program_shape shape_of(u64 seed)
{
    switch (seed % 4)
    {
    case 0:
        return {.max_depth = 1, .max_statements = 3, .max_nodes = 20};
    case 1:
        return {.max_depth = 2, .max_statements = 3, .max_nodes = 40};
    case 2:
        return {.max_depth = 3, .max_statements = 4, .max_nodes = 60};
    default:
        return {.max_depth = 4, .max_statements = 5, .max_nodes = 120};
    }
}

/// The first few failures among the seeds `[0, count)`, and how many there were.
struct sweep
{
    cc::string failures;
    int failed = 0;
};

sweep run_seeds(checked_module const& m, u64 count, legalize_options const& options = {})
{
    auto result = sweep();
    for (auto seed = u64(0); seed < count; ++seed)
    {
        auto const failure = differential_failure(m, seed, shape_of(seed), options);
        if (failure.empty())
            continue;
        if (++result.failed <= 3)
            result.failures += failure;
    }
    return result;
}
} // namespace

TEST("sgl legalize - random structured programs behave the same once they are core")
{
    auto const checked = flat_test_module();
    auto const result = run_seeds(checked.module, nx::is_thorough() ? 20'000 : 400);
    // A failure names its seed and shape: `differential_failure(m, seed, shape)` reproduces it.
    CHECK(result.failures == "");
    CHECK(result.failed == 0);
}

TEST("sgl legalize - the random programs exercise every rule")
{
    auto const checked = flat_test_module();
    auto const& m = checked.module;

    auto with_once = 0;
    auto with_flag = 0;
    auto with_pin = 0;
    auto with_result = 0;
    auto with_prints = 0;
    auto with_eval = 0;
    auto with_dropped_eval = 0;
    for (auto seed = u64(0); seed < 200; ++seed)
    {
        auto const structured = random_program(m, seed, shape_of(seed));
        auto const text = dump_entry_point(m, legalize(m, structured));
        with_once += text.contains("(once") ? 1 : 0;
        with_flag += text.contains("_left :") || text.contains("_continued :") ? 1 : 0;
        with_pin += text.contains("_before :") ? 1 : 0;
        with_result += text.contains("_result :") ? 1 : 0;
        with_prints += interpret(m, structured, test_inputs(m)).trace.size() >= 2 ? 1 : 0;
        // an eval the legalizer kept, and one whose value was a block, so that nothing but its statements is left
        auto const before = dump_entry_point(m, structured);
        with_eval += text.contains("(eval ") ? 1 : 0;
        with_dropped_eval += before.contains("(eval (block ") ? 1 : 0;
    }
    // A generator that stopped producing one of these would leave its rule untested without a single red test.
    CHECK(with_once >= 20);
    CHECK(with_flag >= 20);
    CHECK(with_pin >= 10);
    CHECK(with_result >= 40);
    CHECK(with_prints >= 100);
    CHECK(with_eval >= 20);
    CHECK(with_dropped_eval >= 10);
}

TEST("sgl legalize - the differential test has teeth: without pinning some seed fails")
{
    auto const checked = flat_test_module();
    auto const result = run_seeds(checked.module, 400, {.skip_pinning = true});
    CHECK(result.failed > 0);
    CHECK(result.failures.contains("the two forms behave differently"));
}

TEST("sgl legalize - the differential test has teeth: without the flag tests some seed fails")
{
    auto const checked = flat_test_module();
    auto const result = run_seeds(checked.module, 400, {.skip_flag_tests = true});
    CHECK(result.failed > 0);
    CHECK(result.failures.contains("the two forms behave differently"));
}

TEST("sgl legalize - the same seed gives the same program")
{
    auto const checked = flat_test_module();
    CHECK(random_program(checked.module, 7) == random_program(checked.module, 7));
    CHECK(!(random_program(checked.module, 7) == random_program(checked.module, 8)));
}
