#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>
#include <shaped-xr/impl/capabilities.hh>

// What a capability asks of a runtime decides whether an instance can even be created, so it is pinned here, where a
// test can reach it without one.

namespace
{
constexpr sxr::capability every_capability[]
    = {sxr::capability::passthrough,  sxr::capability::environment_depth, sxr::capability::scene_planes,
       sxr::capability::scene_meshes, sxr::capability::anchors,           sxr::capability::persistent_anchors,
       sxr::capability::hit_tests};

[[nodiscard]] bool has(cc::span<char const* const> names, cc::string_view name)
{
    for (auto const* n : names)
        if (cc::string_view(n) == name)
            return true;
    return false;
}
} // namespace

TEST("shaped-xr capabilities - every capability has a name and at least one extension")
{
    for (auto const c : every_capability)
    {
        CHECK(cc::string_view(sxr::impl::name_of(c)) != "unknown");
        CHECK(!sxr::impl::required_extensions(c).empty());
    }
}

TEST("shaped-xr capabilities - an extension shared by two capabilities is enabled once")
{
    sxr::capability const wanted[] = {sxr::capability::anchors, sxr::capability::persistent_anchors};
    auto const extensions = sxr::impl::extensions_of(wanted);

    auto copies = 0;
    for (auto const* e : extensions)
        copies += cc::string_view(e) == "XR_FB_spatial_entity" ? 1 : 0;
    CHECK(copies == 1);
    CHECK(extensions.size() == 3);
}

TEST("shaped-xr capabilities - nothing wanted enables nothing")
{
    CHECK(sxr::impl::extensions_of({}).empty());
}

TEST("shaped-xr capabilities - a hit test brings the future its raycaster is created through")
{
    CHECK(has(sxr::impl::required_extensions(sxr::capability::hit_tests), "XR_EXT_future"));
}

TEST("shaped-xr capabilities - a listed extension is not enough where the system says no")
{
    auto const none = sxr::impl::system_support();
    CHECK(!sxr::impl::is_supported(sxr::capability::environment_depth, none));
    CHECK(!sxr::impl::is_supported(sxr::capability::scene_planes, none));
    CHECK(!sxr::impl::is_supported(sxr::capability::scene_meshes, none));
    CHECK(!sxr::impl::is_supported(sxr::capability::anchors, none));
    CHECK(!sxr::impl::is_supported(sxr::capability::persistent_anchors, none));
    CHECK(!sxr::impl::is_supported(sxr::capability::hit_tests, none));
}

TEST("shaped-xr capabilities - passthrough has no system answer, so its listing decides")
{
    CHECK(sxr::impl::is_supported(sxr::capability::passthrough, {}));
}

TEST("shaped-xr capabilities - persisting needs an entity, finding it again and saving it")
{
    auto support = sxr::impl::system_support{.spatial_entity = true, .space_discovery = true};
    CHECK(sxr::impl::is_supported(sxr::capability::anchors, support));
    CHECK(!sxr::impl::is_supported(sxr::capability::persistent_anchors, support));

    support.space_persistence = true;
    CHECK(sxr::impl::is_supported(sxr::capability::persistent_anchors, support));
}

TEST("shaped-xr blend - opaque needs nothing, and takes no camera layer even where there is one")
{
    auto const choice = sxr::impl::choose_blend(sxr::blend_mode::opaque, {}, true);
    CHECK(choice.shown == sxr::blend_mode::opaque);
    CHECK(!choice.uses_camera_layer);
}

TEST("shaped-xr blend - a camera layer serves alpha blend on a runtime that reports only opaque")
{
    sxr::blend_mode const runtime[] = {sxr::blend_mode::opaque};
    auto const choice = sxr::impl::choose_blend(sxr::blend_mode::alpha_blend, runtime, true);
    CHECK(choice.shown == sxr::blend_mode::alpha_blend);
    CHECK(choice.uses_camera_layer);
}

TEST("shaped-xr blend - a camera layer wins over the runtime's own alpha blend")
{
    sxr::blend_mode const runtime[] = {sxr::blend_mode::opaque, sxr::blend_mode::alpha_blend};
    CHECK(sxr::impl::choose_blend(sxr::blend_mode::alpha_blend, runtime, true).uses_camera_layer);
}

TEST("shaped-xr blend - without a camera layer the runtime's own blending serves")
{
    sxr::blend_mode const runtime[] = {sxr::blend_mode::alpha_blend, sxr::blend_mode::additive};

    auto const alpha = sxr::impl::choose_blend(sxr::blend_mode::alpha_blend, runtime, false);
    CHECK(alpha.shown == sxr::blend_mode::alpha_blend);
    CHECK(!alpha.uses_camera_layer);

    auto const additive = sxr::impl::choose_blend(sxr::blend_mode::additive, runtime, false);
    CHECK(additive.shown == sxr::blend_mode::additive);
    CHECK(!additive.uses_camera_layer);
}

TEST("shaped-xr blend - a camera layer does not make a display additive")
{
    sxr::blend_mode const runtime[] = {sxr::blend_mode::opaque};
    auto const choice = sxr::impl::choose_blend(sxr::blend_mode::additive, runtime, true);
    CHECK(choice.shown == sxr::blend_mode::opaque);
    CHECK(!choice.uses_camera_layer);
}

TEST("shaped-xr blend - what nothing can serve falls back to opaque")
{
    sxr::blend_mode const runtime[] = {sxr::blend_mode::opaque};
    CHECK(sxr::impl::choose_blend(sxr::blend_mode::alpha_blend, runtime, false).shown == sxr::blend_mode::opaque);
}
