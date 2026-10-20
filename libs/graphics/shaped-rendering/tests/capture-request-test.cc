#include <clean-core/platform/environment.hh>
#include <nexus/test.hh>
#include <shaped-rendering/capture.hh>

// What a capture run reads from the environment.
// `accumulate` carries no unit of its own: a path tracer may read it as samples per pixel, a raster app as frames drawn.

TEST("sr - a capture request reads the accumulate target as given", nx::config::exclusive("env"))
{
    auto const on = cc::scoped_environment_variable(sr::capture_request_env_var, "1");
    auto const accumulate = cc::scoped_environment_variable(sr::capture_accumulate_env_var, "256");
    auto const size = cc::scoped_environment_variable(sr::capture_size_env_var, "96x64");

    auto const req = sr::capture_request::from_environment();
    CHECK(req.active);
    CHECK(req.accumulate == 256);
    CHECK(req.size == tg::vec2i(96, 64));
}

TEST("sr - a capture request keeps its default accumulate target for a value it cannot use", nx::config::exclusive("env"))
{
    nx::expect_warning("capture: ignoring unparseable", nx::exactly(2));

    auto const on = cc::scoped_environment_variable(sr::capture_request_env_var, "1");
    auto const defaults = sr::capture_request{};
    {
        auto const accumulate = cc::scoped_environment_variable(sr::capture_accumulate_env_var, "lots");
        CHECK(sr::capture_request::from_environment().accumulate == defaults.accumulate);
    }
    {
        // 0 would settle on the first frame, before anything accumulated at all.
        auto const accumulate = cc::scoped_environment_variable(sr::capture_accumulate_env_var, "0");
        CHECK(sr::capture_request::from_environment().accumulate == defaults.accumulate);
    }
}

TEST("sr - a capture request is inactive unless asked for", nx::config::exclusive("env"))
{
    auto const off = cc::scoped_environment_variable(sr::capture_request_env_var, {});
    auto const accumulate = cc::scoped_environment_variable(sr::capture_accumulate_env_var, "8");

    // Nothing else is read without the request, so its fields stay the defaults.
    auto const req = sr::capture_request::from_environment();
    CHECK(!req.active);
    CHECK(req.accumulate == sr::capture_request{}.accumulate);
}
