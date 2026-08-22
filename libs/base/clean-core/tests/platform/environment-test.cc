#include <clean-core/common/utility.hh>
#include <clean-core/platform/environment.hh>
#include <nexus/test.hh>

// The reading rules — what counts as absent, and what counts as yes — and the writing ones under them.
//
// The reading tests pin the parsing against what is reliably there rather than against a fixture, which is
// what they did before there was any way to write a variable at all.

TEST("cc environment - an unset variable is absent and not a flag")
{
    auto const name = cc::string_view("SC_A_VARIABLE_NOBODY_SETS_7f3a");
    CHECK(!cc::environment_variable(name).has_value());
    CHECK(!cc::is_environment_flag_set(name));
}

TEST("cc environment - PATH is set on every platform we build for")
{
    // The one variable that is reliably present, so the positive path is covered without setting anything.
    auto const path = cc::environment_variable("PATH");
    CHECK(path.has_value());
    CHECK(!path.value().empty());
    CHECK(cc::is_environment_flag_set("PATH")); // a non-empty value that is not one of the negatives
}

// Writing is process-global, so everything below shares the tag the console tests already hold for the
// environment — a separate one would let the two suites race over the same table.

TEST("cc environment - a value set is a value read back", exclusive("cc-console-color"))
{
    auto const name = cc::string_view("CC_ENVIRONMENT_TEST_VALUE");
    CC_DEFER
    {
        cc::unset_environment_variable(name);
    };

    cc::set_environment_variable(name, "hello");
    auto const value = cc::environment_variable(name);
    REQUIRE(value.has_value());
    CHECK(value.value() == "hello");

    SECTION("setting again replaces rather than appending")
    {
        cc::set_environment_variable(name, "goodbye");
        CHECK(cc::environment_variable(name).value() == "goodbye");
    }

    SECTION("unsetting makes it absent again")
    {
        cc::unset_environment_variable(name);
        CHECK(!cc::environment_variable(name).has_value());
    }

    SECTION("an empty value is the same as unsetting, because reading already treats it that way")
    {
        cc::set_environment_variable(name, "");
        CHECK(!cc::environment_variable(name).has_value());
    }
}

TEST("cc environment - unsetting something that was never set is fine", exclusive("cc-console-color"))
{
    cc::unset_environment_variable("CC_ENVIRONMENT_TEST_NEVER_SET");
    CHECK(!cc::environment_variable("CC_ENVIRONMENT_TEST_NEVER_SET").has_value());
}

TEST("cc environment - the scoped form puts back exactly what was there", exclusive("cc-console-color"))
{
    auto const name = cc::string_view("CC_ENVIRONMENT_TEST_SCOPED");
    CC_DEFER
    {
        cc::unset_environment_variable(name);
    };

    SECTION("a variable that was unset goes back to unset")
    {
        {
            cc::scoped_environment_variable const scope(name, "inside");
            CHECK(cc::environment_variable(name).value() == "inside");
        }

        CHECK(!cc::environment_variable(name).has_value());
    }

    SECTION("a variable that had a value gets that value back")
    {
        cc::set_environment_variable(name, "outside");
        {
            cc::scoped_environment_variable const scope(name, "inside");
            CHECK(cc::environment_variable(name).value() == "inside");
        }

        CHECK(cc::environment_variable(name).value() == "outside");
    }

    SECTION("scopes nest, restoring in destruction order")
    {
        cc::set_environment_variable(name, "outer");
        {
            cc::scoped_environment_variable const first(name, "middle");
            {
                cc::scoped_environment_variable const second(name, "inner");
                CHECK(cc::environment_variable(name).value() == "inner");
            }

            CHECK(cc::environment_variable(name).value() == "middle");
        }

        CHECK(cc::environment_variable(name).value() == "outer");
    }

    SECTION("an empty value unsets for the scope, and restores after it")
    {
        cc::set_environment_variable(name, "outside");
        {
            cc::scoped_environment_variable const scope(name, "");
            CHECK(!cc::environment_variable(name).has_value());
        }

        CHECK(cc::environment_variable(name).value() == "outside");
    }
}
