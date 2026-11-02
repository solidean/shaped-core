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

TEST("sr - a capture request reads an iteration count, and none when unset", nx::config::exclusive("env"))
{
    auto const on = cc::scoped_environment_variable(sr::capture_request_env_var, "1");
    CHECK(sr::capture_request::from_environment().iterations == 0);

    auto const iterations = cc::scoped_environment_variable(sr::capture_iterations_env_var, "3");
    CHECK(sr::capture_request::from_environment().iterations == 3);
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

TEST("sr - a capture request reads a sequence and its clock", nx::config::exclusive("env"))
{
    auto const on = cc::scoped_environment_variable(sr::capture_request_env_var, "1");
    auto const frames = cc::scoped_environment_variable(sr::capture_frames_env_var, "90");
    auto const fps = cc::scoped_environment_variable(sr::capture_fps_env_var, "60");
    auto const time = cc::scoped_environment_variable(sr::capture_time_env_var, "1.5");
    auto const stride = cc::scoped_environment_variable(sr::capture_stride_env_var, "10");

    auto const req = sr::capture_request::from_environment();
    CHECK(req.is_sequence());
    CHECK(req.frames == 90);
    CHECK(req.fps == 60.0);
    CHECK(req.stride == 10);
    CHECK(req.seconds_at(0) == 1.5);
    CHECK(req.seconds_at(30) == 2.0);
}

TEST("sr - a capture request without frames is a still at its start time", nx::config::exclusive("env"))
{
    nx::expect_warning("capture: ignoring unparseable", nx::exactly(2));

    auto const on = cc::scoped_environment_variable(sr::capture_request_env_var, "1");
    auto const time = cc::scoped_environment_variable(sr::capture_time_env_var, "2.25");
    // a stride of 0 would write nothing, and a negative frame count means nothing
    auto const stride = cc::scoped_environment_variable(sr::capture_stride_env_var, "0");
    auto const frames = cc::scoped_environment_variable(sr::capture_frames_env_var, "-3");

    auto const req = sr::capture_request::from_environment();
    CHECK(!req.is_sequence());
    CHECK(req.stride == 1);
    CHECK(req.seconds_at(0) == 2.25);
}

TEST("sr - a sequence frame's path substitutes the index the way ffmpeg reads it")
{
    CHECK(sr::capture_frame_path("out/frame_%05d.png", 7) == "out/frame_00007.png");
    CHECK(sr::capture_frame_path("out/frame_%d.png", 123) == "out/frame_123.png");
    CHECK(sr::capture_frame_path("out/f%03d.jpg", 12345) == "out/f12345.jpg"); // a width is a minimum, never a cut

    // without a placeholder the index goes before the extension, which is what picks the format
    CHECK(sr::capture_frame_path("out/video.png", 4) == "out/video_00004.png");
    CHECK(sr::capture_frame_path("out.d/video", 4) == "out.d/video_00004");
    // a percent sign that is no placeholder is kept as it stands
    CHECK(sr::capture_frame_path("out/100%/a.png", 1) == "out/100%/a_00001.png");
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
