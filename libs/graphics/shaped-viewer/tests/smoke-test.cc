#include <nexus/test.hh>
#include <shaped-viewer/all.hh>

// Confirms the library builds, links, and its umbrella header compiles.
// The feature tests live alongside the features, in the files next to this one.
TEST("sv smoke - links")
{
    CHECK(true);
}

// The shader library is process-wide rather than a viewer's, because a generated material permutation is compiled from the render
// path, which has no viewer to reach back to.
//
// Deliberately order-independent: this binary's GPU tests reach the same library through sv_test::shared_env, and whichever runs first is the one that builds it.
// What is pinned here is that everyone lands on that one library, not who made it.
TEST("sv - the shader library is created once and shared")
{
    auto const first = sv::acquire_shader_library();
    REQUIRE(first.has_value());
    CHECK(first.value() != nullptr);

    CHECK(sv::acquire_shader_library().value() == first.value());

    // Clearing the hook does not un-cache what it already answered with — a viewer already compiling through it keeps working.
    sv::set_acquire_shader_library({});
    CHECK(sv::acquire_shader_library().value() == first.value());
}
