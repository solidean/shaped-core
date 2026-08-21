#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/record/system.hh>
#include <clean-core/record/trace.hh>
#include <nexus/rec.hh>
#include <nexus/test.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

// What a test can ask about its own recording.
//
// These run under the recorder nx::run stood up, so they exercise the real bucketing rather than a fixture's.
// A run started with --no-recording has no recorder, so every recorder here reports unattached.
// Those tests SKIP rather than fail: the flag exists to time the tests themselves, and it would be a poor flag that
// broke the suite it is meant to measure.

TEST("test recording - a test sees what it recorded, and nothing before it", nx::config::recorded)
{
    auto rec = nx::test_recording();
    if (!rec.is_attached())
        SKIP("the run has no recorder (--no-recording)");

    CC_RECORD_MARK("first-mark");
    rec.sync();

    CHECK(rec.all().count("first-mark") == 1);
    CHECK(rec.all().count("never-recorded") == 0);
}

TEST("test recording - sync returns the delta, all accumulates", nx::config::recorded)
{
    auto rec = nx::test_recording();
    if (!rec.is_attached())
        SKIP("the run has no recorder (--no-recording)");

    CC_RECORD_MARK("early");
    rec.sync();

    CC_RECORD_MARK("late");
    auto const since = rec.sync();

    // The delta is what arrived in between, which is what makes "did X happen in THIS window" answerable.
    CHECK(since.count("late") == 1);
    CHECK(since.count("early") == 0);

    // ... and the accumulation still holds both, so "did X ever happen" needs no bookkeeping of your own.
    CHECK(rec.all().count("early") == 1);
    CHECK(rec.all().count("late") == 1);
}

TEST("test recording - a log message and a value both land in the bucket", nx::config::recorded)
{
    auto rec = nx::test_recording();
    if (!rec.is_attached())
        SKIP("the run has no recorder (--no-recording)");

    CC_LOG_INFO("a message from the test");
    CC_RECORD("vertex_count", 1024);
    rec.sync();

    CHECK(rec.all().first_value("vertex_count").value() == 1024);
    CHECK(rec.all().messages().size() >= 1);
}

TEST("test recording - a syncing a second time with nothing new returns nothing", nx::config::recorded)
{
    auto rec = nx::test_recording();
    if (!rec.is_attached())
        SKIP("the run has no recorder (--no-recording)");

    CC_RECORD_MARK("once");
    rec.sync();

    auto const empty = rec.sync();
    CHECK(empty.count("once") == 0);
    CHECK(rec.all().count("once") == 1);
}

TEST("test recording - a test that did not opt in records into nothing")
{
    auto rec = nx::test_recording();

    // No trace is minted for it, so there is no bucket and nothing to attach to.
    CHECK(!rec.is_attached());

    CC_RECORD_MARK("goes-nowhere");
    CHECK(rec.sync().empty());
}

TEST("test recording - a nested test is its own bucket only if it opted in", no_scheduler, nx::config::recorded)
{
    auto rec = nx::test_recording();
    if (!rec.is_attached())
        SKIP("the run has no recorder (--no-recording)");

    CC_RECORD_MARK("outer-mark");

    auto bucketed = nx::config::cfg{};
    bucketed.recorded = true;

    nx::test_registry reg;
    reg.add_declaration("inner-bucketed", bucketed,
                        []
                        {
                            CC_RECORD_MARK("inner-bucketed-mark");
                            CHECK(true); // nexus fails a test that checks nothing
                        });
    reg.add_declaration("inner-plain", {},
                        []
                        {
                            CC_RECORD_MARK("inner-plain-mark");
                            CHECK(true);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});
    CHECK(exec.count_failed_tests() == 0);

    rec.sync();

    CHECK(rec.all().count("outer-mark") == 1);

    // The one that opted in minted its own trace, so its events are filed under that and not under this test.
    CHECK(rec.all().count("inner-bucketed-mark") == 0);

    // The one that did not mints nothing, so it stays under whatever context was already in effect — this test's.
    // That is the honest answer rather than a gap: the work really did run under this test.
    CHECK(rec.all().count("inner-plain-mark") == 1);
}

TEST("test recording - --record buckets a test that never asked", no_scheduler)
{
    // The flag is a run-level override of the per-test default, so this drives the real CLI path rather than setting
    // the field by hand.
    char a0[] = "nexus-test";
    char a1[] = "--record";
    char* argv[] = {a0, a1};
    auto config = nx::test_schedule_config::create_from_args(2, argv);
    CHECK(config.record_all);

    nx::test_registry reg;
    reg.add_declaration("inner-unasked", {},
                        []
                        {
                            // No nx::config::recorded on this one, and it still gets a bucket.
                            auto inner = nx::test_recording();
                            CHECK(inner.is_attached());

                            CC_RECORD_MARK("forced-mark");
                            inner.sync();
                            CHECK(inner.all().count("forced-mark") == 1);
                        });

    auto schedule = nx::test_schedule::create(config, reg);
    auto exec = nx::execute_tests(schedule, config);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);
}
