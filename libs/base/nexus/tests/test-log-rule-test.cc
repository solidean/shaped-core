#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/record/async_scope.hh>
#include <clean-core/record/system.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/impl/rec_session.hh>
#include <nexus/test.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>
#include <nexus/tests/thread_scope.hh>

#include <thread>

// The log rule, judged on nested runs: each inner test's verdict is data here rather than a failure of this binary.
// Every inner run is judged inside its own execute_tests, so none of what it logs reaches the outer run's verdict.

namespace
{
nx::test_schedule_execution run_one(cc::unique_function<void()> body, nx::config::cfg cfg = {})
{
    nx::test_registry reg;
    reg.add_declaration("subject", cfg, cc::move(body));
    auto const schedule = nx::test_schedule::create({}, reg);
    return nx::execute_tests(schedule, {});
}

[[nodiscard]] bool mentions(cc::span<nx::test_error const> errors, cc::string_view text)
{
    auto found = false;
    for (auto const& e : errors)
        found |= e.expr.contains(text) || e.expanded.contains(text);
    return found;
}

[[nodiscard]] bool has_recorder()
{
    return nx::impl::run_recording_active();
}
} // namespace

TEST("log rule - an undeclared warning fails the test", no_scheduler)
{
    if (!has_recorder())
        SKIP("the run has no recorder (--no-recording)");

    auto const exec = run_one(
        []
        {
            CC_LOG_WARNING("log rule probe: nobody declared this");
            CHECK(true);
        });

    REQUIRE(exec.executions.size() == 1);
    CHECK(exec.count_failed_tests() == 1);
    CHECK(mentions(exec.executions[0].root.errors, "undeclared warning"));
    CHECK(mentions(exec.executions[0].root.errors, "nobody declared this"));
}

TEST("log rule - info and below never count", no_scheduler)
{
    if (!has_recorder())
        SKIP("the run has no recorder (--no-recording)");

    auto const exec = run_one(
        []
        {
            CC_LOG_INFO("log rule probe: informational");
            CHECK(true);
        });

    CHECK(exec.count_failed_tests() == 0);
}

TEST("log rule - an expectation passes when met and fails when not", no_scheduler)
{
    if (!has_recorder())
        SKIP("the run has no recorder (--no-recording)");

    auto const met = run_one(
        []
        {
            CC_LOG_WARNING("log rule probe: ring of {} bytes did not fit", 65536);
            nx::expect_warning("ring of * bytes did not fit"); // after the line: the verdict comes at the end
            CHECK(true);
        });
    CHECK(met.count_failed_tests() == 0);

    auto const missing = run_one(
        []
        {
            nx::expect_error("log rule probe: never logged");
            CHECK(true);
        });
    CHECK(missing.count_failed_tests() == 1);
    REQUIRE(missing.executions.size() == 1);
    CHECK(mentions(missing.executions[0].root.errors, "logged 0"));

    // The level is part of the match, so an error does not meet an expected warning.
    auto const wrong_level = run_one(
        []
        {
            nx::expect_warning("log rule probe: level");
            CC_LOG_ERROR("log rule probe: level");
            CHECK(true);
        });
    CHECK(wrong_level.count_failed_tests() == 1);
}

TEST("log rule - exactly counts, and a verbatim wildcard character still matches", no_scheduler)
{
    if (!has_recorder())
        SKIP("the run has no recorder (--no-recording)");

    auto const twice = run_one(
        []
        {
            nx::expect_warning("printed twice? no", nx::exactly(2));
            CC_LOG_WARNING("log rule probe: printed twice? no, once");
            CHECK(true);
        });
    CHECK(twice.count_failed_tests() == 1);

    auto const once = run_one(
        []
        {
            nx::expect_warning("printed twice? no", nx::exactly(1));
            CC_LOG_WARNING("log rule probe: printed twice? no, once");
            CHECK(true);
        });
    CHECK(once.count_failed_tests() == 0);
}

TEST("log rule - a declaration covers its own section pass, and one above the sections covers all", no_scheduler)
{
    if (!has_recorder())
        SKIP("the run has no recorder (--no-recording)");

    auto const in_section = run_one(
        []
        {
            SECTION("a")
            {
                nx::allow_warnings("log rule probe: sectioned");
                CC_LOG_WARNING("log rule probe: sectioned");
                CHECK(true);
            }
            SECTION("b")
            {
                CC_LOG_WARNING("log rule probe: sectioned");
                CHECK(true);
            }
        });

    REQUIRE(in_section.executions.size() == 1);
    auto const& root = in_section.executions[0].root;
    CHECK(root.is_considered_failing);
    REQUIRE(root.subsections.size() == 2);
    CHECK(!root.subsections[0].is_considered_failing);
    CHECK(root.subsections[1].is_considered_failing);
    CHECK(mentions(root.subsections[1].errors, "undeclared warning"));

    auto const above = run_one(
        []
        {
            nx::allow_warnings("log rule probe: sectioned");
            SECTION("a")
            {
                CC_LOG_WARNING("log rule probe: sectioned");
                CHECK(true);
            }
            SECTION("b")
            {
                CC_LOG_WARNING("log rule probe: sectioned");
                CHECK(true);
            }
        });
    CHECK(above.count_failed_tests() == 0);
}

TEST("log rule - a domain narrows a declaration", no_scheduler)
{
    if (!has_recorder())
        SKIP("the run has no recorder (--no-recording)");

    auto const other_domain = run_one(
        []
        {
            nx::allow_warnings("log rule probe: domain", "some-other-domain");
            CC_LOG_WARNING("log rule probe: domain");
            CHECK(true);
        });
    CHECK(other_domain.count_failed_tests() == 1);

    auto const same_domain = run_one(
        []
        {
            nx::allow_warnings("log rule probe: domain", "default");
            CC_LOG_WARNING("log rule probe: domain");
            CHECK(true);
        });
    CHECK(same_domain.count_failed_tests() == 0);
}

TEST("log rule - allow_logs on the declaration waives a level for the whole test", no_scheduler)
{
    if (!has_recorder())
        SKIP("the run has no recorder (--no-recording)");

    auto const warnings_only = nx::impl::merge_config(nx::config::allow_logs(cc::rec::level::warning));
    auto const exec = run_one(
        []
        {
            CC_LOG_WARNING("log rule probe: waived");
            CHECK(true);
        },
        warnings_only);
    CHECK(exec.count_failed_tests() == 0);

    auto const still_error = run_one(
        []
        {
            CC_LOG_ERROR("log rule probe: an error is above the waiver");
            CHECK(true);
        },
        warnings_only);
    CHECK(still_error.count_failed_tests() == 1);
}

TEST("log rule - a record inside a library's async scope still reaches its test", no_scheduler)
{
    if (!has_recorder())
        SKIP("the run has no recorder (--no-recording)");

    auto const exec = run_one(
        []
        {
            CC_RECORD_ASYNC_SCOPE("log-rule-library-work");
            CC_LOG_WARNING("log rule probe: inside a scope that replaced the trace");
            CHECK(true);
        });

    REQUIRE(exec.executions.size() == 1);
    CHECK(exec.count_failed_tests() == 1);
    CHECK(mentions(exec.executions[0].root.errors, "replaced the trace"));
}

TEST("log rule - a thread attributed to the test reports to it", no_scheduler)
{
    if (!has_recorder())
        SKIP("the run has no recorder (--no-recording)");
    if (CC_HAS_THREADS == 0)
        SKIP("needs a second thread");

    auto const exec = run_one(
        []
        {
            nx::expect_warning("log rule probe: from a worker");
            auto worker
                = std::thread(nx::attributed_to_current_test([] { CC_LOG_WARNING("log rule probe: from a worker"); }));
            worker.join();
            CHECK(true);
        });
    CHECK(exec.count_failed_tests() == 0);
}

TEST("log rule - an async body's record lands on its test from whichever worker ran it", no_scheduler)
{
    if (!has_recorder())
        SKIP("the run has no recorder (--no-recording)");

    nx::test_registry reg;
    reg.add_async_declaration("subject", {},
                              [](nx::impl::async_test_sink& sink)
                              {
                                  nx::impl::submit_test_async(sink,
                                                              []() -> cc::shared_async<cc::unit>
                                                              {
                                                                  co_await cc::async_yield();
                                                                  CC_LOG_WARNING("log rule probe: after a suspend");
                                                                  CHECK(true);
                                                                  co_return;
                                                              }());
                              });

    auto config = nx::test_schedule_config{};
    config.jobs = 4;
    auto const schedule = nx::test_schedule::create({}, reg);
    auto const exec = nx::execute_tests(schedule, config);

    REQUIRE(exec.executions.size() == 1);
    CHECK(exec.count_failed_tests() == 1);
    CHECK(mentions(exec.executions[0].root.errors, "after a suspend"));
}

TEST("log rule - a failed check is not also an undeclared error", no_scheduler)
{
    if (!has_recorder())
        SKIP("the run has no recorder (--no-recording)");

    auto const exec = run_one([] { CHECK(1 == 2); });

    REQUIRE(exec.executions.size() == 1);
    CHECK(exec.count_failed_checks() == 1);
    CHECK(!mentions(exec.executions[0].root.errors, "undeclared"));
}

// Registered for this whole binary, which is what the allowance is: no test below declares the record it matches.
NX_ALLOW_LOGS(cc::rec::level::warning, "", "log rule probe: allowed across the binary");

TEST("log rule - NX_ALLOW_LOGS allows a record in every test of the binary", no_scheduler)
{
    if (!has_recorder())
        SKIP("the run has no recorder (--no-recording)");

    auto const exec = run_one(
        []
        {
            CC_LOG_WARNING("log rule probe: allowed across the binary");
            CHECK(true);
        });
    CHECK(exec.count_failed_tests() == 0);

    // At or below its level only: the same text as an error is still undeclared.
    auto const as_error = run_one(
        []
        {
            CC_LOG_ERROR("log rule probe: allowed across the binary");
            CHECK(true);
        });
    CHECK(as_error.count_failed_tests() == 1);
}

TEST("log rule - a recorded test that fails only by the rule keeps its recording", no_scheduler)
{
    if (!has_recorder())
        SKIP("the run has no recorder (--no-recording)");

    auto const recorded = nx::impl::merge_config(nx::config::recorded);

    // Its bucket closes before the verdict, so it is kept undecided until the rule has judged the test.
    auto const failing = run_one(
        []
        {
            CC_RECORD_MARK("log rule probe: recorded before the warning");
            CC_LOG_WARNING("log rule probe: fails a recorded test");
            CHECK(true);
        },
        recorded);
    REQUIRE(failing.executions.size() == 1);
    CHECK(failing.count_failed_tests() == 1);
    REQUIRE(failing.executions[0].record_trace != 0);
    CHECK(nx::impl::take_test_bucket(cc::rec::trace_id(failing.executions[0].record_trace))
              .count("log rule probe: recorded before the warning")
          == 1);

    // A passing one still lets its events go.
    auto const passing = run_one(
        []
        {
            CC_RECORD_MARK("log rule probe: a passing recorded test");
            CHECK(true);
        },
        recorded);
    REQUIRE(passing.executions.size() == 1);
    CHECK(passing.count_failed_tests() == 0);
    CHECK(nx::impl::take_test_bucket(cc::rec::trace_id(passing.executions[0].record_trace)).empty());
}
