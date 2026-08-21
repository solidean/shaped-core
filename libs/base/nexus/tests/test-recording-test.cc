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
// A binary run without nx::run has no recorder, so every recorder here reports unattached and the checks below would
// be vacuous — which is why each one that matters asserts is_attached() first.

TEST("test recording - a test sees what it recorded, and nothing before it")
{
    auto rec = nx::test_recording();
    REQUIRE(rec.is_attached());

    CC_RECORD_MARK("first-mark");
    rec.sync();

    CHECK(rec.all().count("first-mark") == 1);
    CHECK(rec.all().count("never-recorded") == 0);
}

TEST("test recording - sync returns the delta, all accumulates")
{
    auto rec = nx::test_recording();
    REQUIRE(rec.is_attached());

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

TEST("test recording - a log message and a value both land in the bucket")
{
    auto rec = nx::test_recording();
    REQUIRE(rec.is_attached());

    CC_LOG_INFO("a message from the test");
    CC_RECORD("vertex_count", 1024);
    rec.sync();

    CHECK(rec.all().first_value("vertex_count").value() == 1024);
    CHECK(rec.all().messages().size() >= 1);
}

TEST("test recording - a syncing a second time with nothing new returns nothing")
{
    auto rec = nx::test_recording();
    REQUIRE(rec.is_attached());

    CC_RECORD_MARK("once");
    rec.sync();

    auto const empty = rec.sync();
    CHECK(empty.count("once") == 0);
    CHECK(rec.all().count("once") == 1);
}

TEST("test recording - an opted-out test records into nothing", nx::config::no_recording)
{
    auto rec = nx::test_recording();

    // No trace is minted for it, so there is no bucket and nothing to attach to.
    CHECK(!rec.is_attached());

    CC_RECORD_MARK("goes-nowhere");
    CHECK(rec.sync().empty());
}

TEST("test recording - a nested test's events belong to the nested test", no_scheduler)
{
    auto rec = nx::test_recording();
    REQUIRE(rec.is_attached());

    CC_RECORD_MARK("outer-mark");

    nx::test_registry reg;
    reg.add_declaration("inner", {},
                        []
                        {
                            CC_RECORD_MARK("inner-mark");
                            CHECK(true); // nexus fails a test that checks nothing
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});
    CHECK(exec.count_failed_tests() == 0);

    rec.sync();

    // The nested test ran under this one's ambient chain and still minted its own trace, so the segment it produced
    // is filed under that trace and not this one.
    // Attribution nests logically; buckets do not.
    CHECK(rec.all().count("outer-mark") == 1);
    CHECK(rec.all().count("inner-mark") == 0);
}
