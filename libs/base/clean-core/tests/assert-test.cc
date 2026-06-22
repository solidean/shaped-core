#include <clean-core/assert-handler.hh>
#include <clean-core/assertf.hh>

#include <nexus/test.hh>

#include <optional>
#include <vector>


TEST("assertions - failing assertion calls handler with correct payload")
{
    std::optional<cc::impl::assertion_info> captured;
    // CAREFUL: this is a bit brittle wrt. formatting but it should be fine
    int const test_line = __LINE__ + 11; // line where CC_ASSERT_ALWAYS is called

    {
        auto handler = cc::impl::scoped_assertion_handler(
            [&](cc::impl::assertion_info const& info)
            {
                captured = info;
                throw 0; // Must throw to prevent abort
            });
        try
        {
            CC_ASSERTF_ALWAYS(false, "hello {}", 42);
        }
        catch (int) // NOLINT(bugprone-empty-catch)
        {
        }
    }

    REQUIRE(captured.has_value());

    // Expression: should contain the stringified condition
    CHECK(!captured->expression.empty());
    CHECK(captured->expression.find("false") != std::string::npos);

    // Message: should be formatted correctly
    CHECK(captured->message == "hello 42");

    // Location: file name should end with this test file
    auto file_name = std::string(captured->location.file_name());
    CHECK(file_name.ends_with("assert-test.cc"));

    // Location: line should be exact
    CHECK(captured->location.line() == test_line);

    // Location: function name should be non-empty
    CHECK(!std::string(captured->location.function_name()).empty());
}

TEST("assertions - passing assertion does not call handler")
{
    bool handler_called = false;
    int counter = 0;

    auto expensive = [&]() -> int
    {
        ++counter;
        return 99;
    };

    {
        auto handler
            = cc::impl::scoped_assertion_handler([&](cc::impl::assertion_info const&) { handler_called = true; });
        CC_ASSERTF_ALWAYS(true, "should not matter {}", expensive());
    }

    CHECK(!handler_called);
    CHECK(counter == 0); // proves message args aren't evaluated on pass
}

TEST("assertions - handler stack is LIFO and nesting works")
{
    std::vector<int> events;

    auto handler_a = cc::impl::scoped_assertion_handler(
        [&](cc::impl::assertion_info const&)
        {
            events.push_back(1); // handler A
            throw 0;
        });

    {
        auto handler_b = cc::impl::scoped_assertion_handler(
            [&](cc::impl::assertion_info const&)
            {
                events.push_back(2); // handler B
                throw 0;
            });

        // Trigger failure with B active
        try
        {
            CC_ASSERT_ALWAYS(false, "first failure");
        }
        catch (int) // NOLINT(bugprone-empty-catch)
        {
        }
    }
    // B is now popped

    // Trigger failure with only A active
    try
    {
        CC_ASSERT_ALWAYS(false, "second failure");
    }
    catch (int) // NOLINT(bugprone-empty-catch)
    {
    }

    REQUIRE(events.size() == 2);
    CHECK(events[0] == 2); // first failure hit handler B
    CHECK(events[1] == 1); // second failure hit handler A
}

TEST("assertions - scoped_assertion_handler pops on scope exit even when handler throws")
{
    std::vector<int> events;
    bool outer_handler_works = false;

    auto outer = cc::impl::scoped_assertion_handler(
        [&](cc::impl::assertion_info const&)
        {
            events.push_back(1);
            outer_handler_works = true;
            throw 0; // Must throw to prevent abort
        });

    struct sentinel_exception
    {
    };

    try
    {
        auto inner = cc::impl::scoped_assertion_handler(
            [&](cc::impl::assertion_info const&)
            {
                events.push_back(2);
                throw sentinel_exception{};
            });

        CC_ASSERT_ALWAYS(false, "trigger inner");
        CHECK(false); // should not reach here
    }
    catch (sentinel_exception const&)
    {
        // Expected: inner handler threw
    }

    // Verify inner handler was called
    REQUIRE(events.size() == 1);
    CHECK(events[0] == 2);

    // Now trigger another failure: outer handler should still work
    outer_handler_works = false;
    try
    {
        CC_ASSERT_ALWAYS(false, "trigger outer");
    }
    catch (int) // NOLINT(bugprone-empty-catch)
    {
    }

    CHECK(outer_handler_works);
    REQUIRE(events.size() == 2);
    CHECK(events[1] == 1); // outer handler was called
}

TEST("assertions - multiple failures produce independent reports")
{
    std::vector<cc::impl::assertion_info> captures;
    int const line1 = __LINE__ + 12;
    int const line2 = __LINE__ + 18;

    {
        auto handler = cc::impl::scoped_assertion_handler(
            [&](cc::impl::assertion_info const& info)
            {
                captures.push_back(info);
                throw 0;
            });
        try
        {
            CC_ASSERT_ALWAYS(false, "first message");
        }
        catch (int) // NOLINT(bugprone-empty-catch)
        {
        }
        try
        {
            CC_ASSERT_ALWAYS(1 > 2, "second message");
        }
        catch (int) // NOLINT(bugprone-empty-catch)
        {
        }
    }

    REQUIRE(captures.size() == 2);

    // First failure
    CHECK(captures[0].expression.find("false") != std::string::npos);
    CHECK(captures[0].message == "first message");
    CHECK(captures[0].location.line() >= line1 - 1);
    CHECK(captures[0].location.line() <= line1 + 1);

    // Second failure
    CHECK(captures[1].expression.find("1") != std::string::npos);
    CHECK(captures[1].expression.find("2") != std::string::npos);
    CHECK(captures[1].message == "second message");
    CHECK(captures[1].location.line() >= line2 - 1);
    CHECK(captures[1].location.line() <= line2 + 1);

    // Line numbers should differ
    CHECK(captures[0].location.line() != captures[1].location.line());
}
