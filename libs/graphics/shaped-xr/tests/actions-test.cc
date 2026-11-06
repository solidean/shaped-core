#include <nexus/test.hh>
#include <shaped-xr/impl/actions.hh>

using namespace cc::primitive_defines;

// Actions are bound before a session begins and OpenXR refuses a bad one outright, so what it would refuse is judged
// on the host first, where a test can reach it without a runtime.

TEST("shaped-xr actions - a name is lowercase letters, digits, dashes, underscores and dots")
{
    CHECK(sxr::impl::is_valid_action_name("grab_cube"));
    CHECK(sxr::impl::is_valid_action_name("menu.open-2"));
    CHECK(!sxr::impl::is_valid_action_name(""));
    CHECK(!sxr::impl::is_valid_action_name("Grab"));
    CHECK(!sxr::impl::is_valid_action_name("grab cube"));
}

TEST("shaped-xr actions - a repeated name, a per-hand binding without a hand, and a mismatched output are refused")
{
    auto const left_trigger = sxr::action_binding{.profile = "/interaction_profiles/oculus/touch_controller",
                                                  .path = "/user/hand/left/input/trigger/value"};
    auto const trigger
        = sxr::action_description{.name = "trigger", .kind = sxr::action_kind::scalar, .bindings = {left_trigger}};
    CHECK(sxr::impl::problem_of(cc::vector<sxr::action_description>{trigger}).empty());
    CHECK(sxr::impl::problem_of(cc::vector<sxr::action_description>{trigger, trigger}).contains("names two actions"));

    auto handless = trigger;
    handless.bindings = {{.profile = left_trigger.profile, .path = "/user/gamepad/input/trigger_left/value"}};
    CHECK(sxr::impl::problem_of(cc::vector<sxr::action_description>{handless}).contains("names no hand"));
    handless.is_per_hand = false;
    CHECK(sxr::impl::problem_of(cc::vector<sxr::action_description>{handless}).empty());

    auto buzz = trigger;
    buzz.name = "buzz";
    buzz.kind = sxr::action_kind::vibration;
    CHECK(sxr::impl::problem_of(cc::vector<sxr::action_description>{buzz}).contains("is an input"));
}

TEST("shaped-xr actions - bindings are grouped a profile at a time, in the order the profiles first appear")
{
    auto const actions = cc::vector<sxr::action_description>{
        {.name = "a",
         .bindings = {{.profile = "p", .path = "/user/hand/left/input/a/click"},
                      {.profile = "q", .path = "/user/hand/left/input/b/click"}}},
        {.name = "b", .bindings = {{.profile = "p", .path = "/user/hand/right/input/a/click"}}},
    };
    auto const groups = sxr::impl::bindings_by_profile(actions);
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].profile == "p");
    REQUIRE(groups[0].actions.size() == 2);
    CHECK(groups[0].actions[0] == 0);
    CHECK(groups[0].actions[1] == 1);
    CHECK(groups[1].profile == "q");
    REQUIRE(groups[1].actions.size() == 1);
    CHECK(groups[1].actions[0] == 0);
}

TEST("shaped-xr actions - the standard set is valid, and binds every input of Quest Touch for both hands")
{
    auto const actions = sxr::standard_controller_actions();
    CHECK(sxr::impl::problem_of(actions) == "");
    auto touch = cc::vector<cc::string>();
    for (auto const& group : sxr::impl::bindings_by_profile(actions))
        if (group.profile == "/interaction_profiles/oculus/touch_controller")
            touch = group.paths;
    for (auto const* const path :
         {"/user/hand/left/input/grip/pose", "/user/hand/right/input/aim/pose", "/user/hand/right/input/trigger/value",
          "/user/hand/left/input/squeeze/value", "/user/hand/left/input/thumbstick", "/user/hand/right/input/a/click",
          "/user/hand/left/input/y/click", "/user/hand/left/input/menu/click", "/user/hand/right/output/haptic"})
    {
        auto is_bound = false;
        for (auto const& p : touch)
            is_bound = is_bound || p == path;
        CHECK(is_bound).dump("path", path);
    }
}
