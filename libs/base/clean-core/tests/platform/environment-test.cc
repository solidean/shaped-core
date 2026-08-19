#include <clean-core/platform/environment.hh>
#include <nexus/test.hh>

// The reading rules, which are the whole surface: what counts as absent, and what counts as yes.
// Setting a variable is not part of the API — process-global and racy — so these pin the parsing against
// what is reliably there rather than against a fixture.

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
