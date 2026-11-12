#include "actions.hh"

#include <clean-core/string/format.hh>

using namespace cc::primitive_defines;

bool sxr::impl::is_valid_action_name(cc::string_view name)
{
    if (name.empty())
        return false;
    for (auto const c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return false;
    return true;
}

cc::string sxr::impl::problem_of(cc::span<action_description const> actions)
{
    for (auto i = isize(0); i < actions.size(); ++i)
    {
        auto const& a = actions[i];
        if (!is_valid_action_name(a.name))
            return cc::format("'{}' is no action name: lowercase letters, digits, '-', '_' and '.'", a.name);
        for (auto j = isize(0); j < i; ++j)
            if (actions[j].name == a.name)
                return cc::format("'{}' names two actions", a.name);
        for (auto const& b : a.bindings)
        {
            auto const path = cc::string_view(b.path);
            if (a.is_per_hand && !path.starts_with("/user/hand/left/") && !path.starts_with("/user/hand/right/"))
                return cc::format("{} is per hand, and {} names no hand", a.name, b.path);
            if ((a.kind == action_kind::vibration) != path.contains("/output/"))
                return cc::format("{} is {}, and {} is an {}", a.name,
                                  a.kind == action_kind::vibration ? "a vibration" : "an input", b.path,
                                  path.contains("/output/") ? "output" : "input");
        }
    }
    return {};
}

cc::vector<sxr::impl::profile_bindings> sxr::impl::bindings_by_profile(cc::span<action_description const> actions)
{
    auto out = cc::vector<profile_bindings>();
    for (auto i = isize(0); i < actions.size(); ++i)
        for (auto const& b : actions[i].bindings)
        {
            auto* group = static_cast<profile_bindings*>(nullptr);
            for (auto& g : out)
                if (g.profile == b.profile)
                    group = &g;
            if (group == nullptr)
            {
                out.push_back({.profile = b.profile});
                group = &out.back();
            }
            group->actions.push_back(i);
            group->paths.push_back(b.path);
        }
    return out;
}

cc::vector<sxr::action_description> sxr::standard_controller_actions()
{
    constexpr auto touch = cc::string_view("/interaction_profiles/oculus/touch_controller");
    constexpr auto simple = cc::string_view("/interaction_profiles/khr/simple_controller");
    // one action bound for both hands, at `suffix` under each hand, on each of `profiles`
    auto const both = [](cc::string_view name, cc::string_view label, action_kind kind, cc::string_view suffix,
                         std::initializer_list<cc::string_view> profiles)
    {
        auto a = action_description{.name = cc::string(name), .label = cc::string(label), .kind = kind};
        for (auto const profile : profiles)
            for (auto const hand : {"left", "right"})
                a.bindings.push_back(
                    {.profile = cc::string(profile), .path = cc::format("/user/hand/{}/{}", hand, suffix)});
        return a;
    };
    auto out = cc::vector<action_description>{
        both("grip", "Grip pose", action_kind::pose, "input/grip/pose", {touch, simple}),
        both("aim", "Aim pose", action_kind::pose, "input/aim/pose", {touch, simple}),
        both("trigger", "Trigger", action_kind::scalar, "input/trigger/value", {touch}),
        both("squeeze", "Squeeze", action_kind::scalar, "input/squeeze/value", {touch}),
        both("thumbstick", "Thumbstick", action_kind::vector2, "input/thumbstick", {touch}),
        both("thumbstick_click", "Thumbstick click", action_kind::boolean, "input/thumbstick/click", {touch}),
        both("haptic", "Vibration", action_kind::vibration, "output/haptic", {touch, simple}),
    };
    // the simple controller's one button stands in for the trigger, and the face buttons differ per hand
    out[2].bindings.push_back({.profile = cc::string(simple), .path = "/user/hand/left/input/select/click"});
    out[2].bindings.push_back({.profile = cc::string(simple), .path = "/user/hand/right/input/select/click"});
    out.push_back({.name = "primary",
                   .label = "A or X",
                   .kind = action_kind::boolean,
                   .bindings = {{.profile = cc::string(touch), .path = "/user/hand/left/input/x/click"},
                                {.profile = cc::string(touch), .path = "/user/hand/right/input/a/click"}}});
    out.push_back({.name = "secondary",
                   .label = "B or Y",
                   .kind = action_kind::boolean,
                   .bindings = {{.profile = cc::string(touch), .path = "/user/hand/left/input/y/click"},
                                {.profile = cc::string(touch), .path = "/user/hand/right/input/b/click"}}});
    out.push_back({.name = "menu",
                   .label = "Menu",
                   .kind = action_kind::boolean,
                   .bindings = {{.profile = cc::string(touch), .path = "/user/hand/left/input/menu/click"},
                                {.profile = cc::string(simple), .path = "/user/hand/left/input/menu/click"},
                                {.profile = cc::string(simple), .path = "/user/hand/right/input/menu/click"}}});
    return out;
}
