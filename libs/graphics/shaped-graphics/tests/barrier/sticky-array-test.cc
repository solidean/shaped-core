#include "../shaders/shader_fixtures.hh"

#include <clean-core/common/macros.hh> // CC_ASSERT_ENABLED
#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/metrics.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>
#include <shaped-graphics/raytracing/raytracing_pipeline.hh>
#include <shaped-graphics/raytracing/raytracing_shader_table.hh>
#include <shaped-graphics/resource/raw_texture.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-shader-library/raytracing_pipeline.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// Array declarations that outlive one op: the arrays of tests/shaders/sgl/sticky_arrays.sgl and sticky_arrays_traced.sgl, met by many dispatches, traces and draws.
// See libs/graphics/shaped-graphics/docs/concepts/barriers.md, "Array bindings".
//
// Most of what these pin is what the list fed the tracker, read off the counts it has recorded so far:
// `array_element_tracks` is the element work an op did, and a skipped application does none.
// A barrier count is read where the backend counts per resource; metal names stages only, and leaves those out.

namespace
{
constexpr int k_words = 128;
constexpr int k_threads = 64;

auto constexpr source_usage = sg::buffer_usage::readonly_buffer | sg::buffer_usage::readwrite_buffer
                            | sg::buffer_usage::copy_dst | sg::buffer_usage::copy_src;

void require_arrays(sg::context const& ctx)
{
    if (!sg_test::shaders_reach(ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx.supports(sg::feature::binding_arrays))
        SKIP("this context has no binding arrays");
}

/// What the list has counted of `s` so far, which a test differences across one op.
[[nodiscard]] i64 recorded(sg::command_list const& cmd, sg::stat s)
{
    return sg::impl::recorded_stats(cmd).values[int(s)];
}

[[nodiscard]] i64 tracks(sg::command_list const& cmd)
{
    return recorded(cmd, sg::stat::array_element_tracks);
}

/// Whether a texture moves between layouts on this backend.
/// Metal's have none, so a layout another declaration asks for writes nothing there, and the first stays current.
[[nodiscard]] bool has_layouts(sg::context const& ctx)
{
    return ctx.backend() != sg::backend_kind::metal;
}

/// What an application of both gallery images tracks where another moved their layouts: both, or nothing without layouts.
[[nodiscard]] i64 reapplied(sg::context const& ctx)
{
    return has_layouts(ctx) ? 2 : 0;
}

/// Words `k * 1000 + i`, so a gather shows which source each thread read, and a write to one shows as a different word.
[[nodiscard]] sg::buffer<i32> make_source(sg::context& ctx, i32 k)
{
    auto data = cc::vector<i32>();
    for (auto i = 0; i < k_words; ++i)
        data.push_back(k * 1000 + i);
    return ctx.persistent.create_buffer_from_data(cc::move(data), source_usage);
}

[[nodiscard]] sg::buffer<i32> make_tray(sg::context& ctx)
{
    return ctx.persistent.create_buffer_from_data(cc::vector<i32>::create_defaulted(k_threads),
                                                  sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
}

[[nodiscard]] sg::buffer<float> make_easel(sg::context& ctx)
{
    return ctx.persistent.create_buffer_from_data(cc::vector<float>::create_defaulted(k_threads),
                                                  sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
}

/// A 1x1 rgba32_float texture whose red channel is `red`, uploaded and submitted before any list of a test records.
[[nodiscard]] sg::texture_2d make_image(sg::context& ctx, float red, sg::texture_usages extra = {})
{
    auto const texture = sg::texture_2d::from_raw(ctx.persistent.create_raw_texture({
        .format = sg::pixel_format::rgba32_float,
        .dimension = sg::texture_dimension::d2,
        .width = 1,
        .height = 1,
        .usage = sg::texture_usage::texture | sg::texture_usage::copy_src | sg::texture_usage::copy_dst | extra,
    }));
    float const texel[] = {red, 0.0f, 0.0f, 1.0f};
    auto up = ctx.create_command_list();
    up->upload.bytes_to_texture(texture.raw(), cc::as_bytes(cc::span<float const>(texel)));
    ctx.submit_command_list(cc::move(up));
    return texture;
}

[[nodiscard]] sg::binding_group_handle shelf_of(sg::command_list& cmd, cc::span<sg::buffer<i32> const> sources)
{
    auto& ctx = cmd.context();
    return ctx.transient.create_binding_group(
        cmd, ctx.cached.acquire_binding_group_layout<shaders::shelf>(),
        shaders::shelf{.sources = {sources[0].as_readonly_buffer(), sources[1].as_readonly_buffer(),
                                   sources[2].as_readonly_buffer()}});
}

/// Binds `kernel`, the shelf at 0 and a tray of `tray` at 1, and gathers: the shape every bindless dispatch has.
void gather(sg::command_list& cmd,
            sg::compute_pipeline const& kernel,
            sg::binding_group const& shelf,
            sg::buffer<i32> const& tray)
{
    auto& ctx = cmd.context();
    auto const tray_group
        = ctx.transient.create_binding_group(cmd, ctx.cached.acquire_binding_group_layout<shaders::tray>(),
                                             shaders::tray{.merged = tray.as_readwrite_buffer()});
    cmd.compute.bind_pipeline(kernel);
    cmd.compute.bind_group(0, shelf);
    cmd.compute.bind_group(1, *tray_group);
    cmd.compute.dispatch_threads(k_threads);
}

/// Adds 1000 to the first 64 words of `target`, through a scalar binding the shelf's declaration cannot see.
void bump(sg::command_list& cmd, sg::compute_pipeline const& kernel, sg::buffer<i32> const& target)
{
    auto& ctx = cmd.context();
    auto const patch = ctx.transient.create_binding_group(cmd, ctx.cached.acquire_binding_group_layout<shaders::patch>(),
                                                          shaders::patch{.target = target.as_readwrite_buffer()});
    cmd.compute.bind_pipeline(kernel);
    cmd.compute.bind_group(0, *patch);
    cmd.compute.dispatch_threads(k_threads);
}

/// Loads texel (0, 0) of the image `id % 2` picks into `easel`, through whichever array `images` is.
void look(sg::command_list& cmd,
          sg::compute_pipeline const& kernel,
          sg::binding_group const& images,
          sg::buffer<float> const& easel)
{
    auto& ctx = cmd.context();
    auto const easel_group
        = ctx.transient.create_binding_group(cmd, ctx.cached.acquire_binding_group_layout<shaders::easel>(),
                                             shaders::easel{.reds = easel.as_readwrite_buffer()});
    cmd.compute.bind_pipeline(kernel);
    cmd.compute.bind_group(0, images);
    cmd.compute.bind_group(1, *easel_group);
    cmd.compute.dispatch_threads(k_threads);
}

[[nodiscard]] cc::vector<sg::array_buffer_access> reads_of_three()
{
    return {{.index = 0, .access = sg::access_flag::shader_read},
            {.index = 1, .access = sg::access_flag::shader_read},
            {.index = 2, .access = sg::access_flag::shader_read}};
}

[[nodiscard]] cc::vector<sg::array_texture_access> images_at(sg::texture_layout first, sg::texture_layout second)
{
    return {{.index = 0, .access = sg::access_flag::shader_read, .layout = first},
            {.index = 1, .access = sg::access_flag::shader_read, .layout = second}};
}

/// What a gather into a tray reads back as, with source 1 `bumped` by the 1000 a bump adds.
void check_gathered(cc::span<i32 const> got, i32 source_one_offset, cc::string_view what)
{
    REQUIRE(got.size() == isize(k_threads));
    for (auto i = 0; i < k_threads; ++i)
    {
        auto const k = i % 3;
        auto const want = k * 1000 + i + (k == 1 ? source_one_offset : 0);
        CHECK(got[i] == want).context(cc::format("{}, thread {}", what, i));
    }
}

/// Turns the context's portability checks off for one test, and back on at its end.
/// A dispatch reading an array element and writing the same buffer through a scalar binding is what WebGPU refuses.
/// The tests that need it write words the read never touches.
class portability_checks_off
{
public:
    explicit portability_checks_off(sg::context& ctx) : _ctx(ctx), _was(ctx.portability_checks())
    {
        _ctx.set_portability_checks(false);
    }
    ~portability_checks_off() { _ctx.set_portability_checks(_was); }

    portability_checks_off(portability_checks_off const&) = delete;
    portability_checks_off& operator=(portability_checks_off const&) = delete;

private:
    sg::context& _ctx;
    bool _was = false;
};

/// Thrown to unwind past a declaration.
struct unwound
{
};
} // namespace

ASYNC_INVOCABLE_TEST("sg - a current array declaration tracks no element at the next op, across pipeline and group "
                     "rebinds",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    require_arrays(*ctx);

    auto const kernel = co_await shaders::sticky_arrays.gather_shelf.acquire_pipeline(*ctx);
    auto const bumper = co_await shaders::sticky_arrays.bump.acquire_pipeline(*ctx);
    sg::buffer<i32> const sources[] = {make_source(*ctx, 0), make_source(*ctx, 1), make_source(*ctx, 2)};
    auto const unrelated = make_source(*ctx, 9);
    auto const first = make_tray(*ctx);
    auto const second = make_tray(*ctx);
    auto const third = make_tray(*ctx);

    auto cmd = ctx->create_command_list();
    auto const shelf = shelf_of(*cmd, sources);
    // The wavefront's shape: declared once, then the pipeline and the shelf bound again before every dispatch.
    auto const declared = cmd->compute.declare_array_buffer_access("shelf.sources", reads_of_three());
    gather(*cmd, *kernel, *shelf, first);
    auto const after_first = tracks(*cmd);
    CHECK(after_first == 3);

    gather(*cmd, *kernel, *shelf, second);
    CHECK(tracks(*cmd) == after_first);

    // A dispatch binding no shelf leaves the declaration dormant, and writes nothing it holds.
    bump(*cmd, *bumper, unrelated);
    gather(*cmd, *kernel, *shelf, third);
    CHECK(tracks(*cmd) == after_first);
    auto const first_back = cmd->download.data_from_buffer(first);
    auto const third_back = cmd->download.data_from_buffer(third);
    ctx->submit_command_list(cc::move(cmd));

    check_gathered(co_await first_back.data(), 0, "the first gather");
    check_gathered(co_await third_back.data(), 0, "the third gather");
}

namespace
{
/// How the worked example's op 2 writes source 1, which the shelf holds: each path a resource can be written through.
enum class member_write_path
{
    scalar_binding,
    copy,
    upload,
};
} // namespace

ASYNC_INVOCABLE_TEST("sg - a write to an array member between two reads re-applies the declaration, then it skips",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    require_arrays(*ctx);

    auto const kernel = co_await shaders::sticky_arrays.gather_shelf.acquire_pipeline(*ctx);
    auto const bumper = co_await shaders::sticky_arrays.bump.acquire_pipeline(*ctx);

    for (auto const path : {member_write_path::scalar_binding, member_write_path::copy, member_write_path::upload})
    {
        auto const name = path == member_write_path::scalar_binding ? "a scalar binding"
                        : path == member_write_path::copy           ? "a copy"
                                                                    : "an upload";
        sg::buffer<i32> const sources[] = {make_source(*ctx, 0), make_source(*ctx, 1), make_source(*ctx, 2)};
        auto const spare = make_source(*ctx, 1);
        sg::buffer<i32> const trays[] = {make_tray(*ctx), make_tray(*ctx), make_tray(*ctx)};

        auto cmd = ctx->create_command_list();
        auto const shelf = shelf_of(*cmd, sources);
        auto deltas = cc::vector<i64>();
        auto barriers = cc::vector<i64>();
        auto const declared = cmd->compute.declare_array_buffer_access("shelf.sources", reads_of_three());
        gather(*cmd, *kernel, *shelf, trays[0]); // op 1: applied, current

        auto const before_write = tracks(*cmd);
        switch (path) // op 2: source 1 written, which ends the application's currency
        {
        case member_write_path::scalar_binding:
            bump(*cmd, *bumper, sources[1]);
            break;
        case member_write_path::copy:
        {
            // The spare's words are source 1's plus 0: the same words, written, so the gather reads the same.
            cmd->copy.buffer_data_region<i32>(
                {.src = spare.raw(), .dst = sources[1].raw(), .count = k_words, .src_offset = 0, .dst_offset = 0});
            break;
        }
        case member_write_path::upload:
        {
            auto words = cc::vector<i32>();
            for (auto i = 0; i < k_words; ++i)
                words.push_back(1000 + i + 1000);
            cmd->upload.data_to_buffer(sources[1].raw(), cc::span<i32 const>(words));
            break;
        }
        }
        CHECK(tracks(*cmd) == before_write).context(name);

        for (auto op = 3; op <= 4; ++op)
        {
            auto const tracked = tracks(*cmd);
            auto const barriers_before = recorded(*cmd, sg::stat::buffer_barriers);
            gather(*cmd, *kernel, *shelf, trays[op - 2]);
            deltas.push_back(tracks(*cmd) - tracked);
            barriers.push_back(recorded(*cmd, sg::stat::buffer_barriers) - barriers_before);
        }
        auto const back = cmd->download.data_from_buffer(trays[2]);
        ctx->submit_command_list(cc::move(cmd));
        auto const counted = ctx->metrics.stats();

        // Op 3 re-applies, meeting the write; op 4 finds it current again and touches nothing.
        CHECK(deltas[0] == 3).context(name);
        CHECK(deltas[1] == 0).context(name);
        if (counted.is_counted(sg::stat::buffer_barriers))
        {
            CHECK(barriers[0] >= 1).context(cc::format("{}: op 3 orders the read after the write", name));
            CHECK(barriers[1] == 0).context(cc::format("{}: op 4 needs nothing", name));
        }
        check_gathered(co_await back.data(), path == member_write_path::copy ? 0 : 1000, name);
    }
}

ASYNC_INVOCABLE_TEST("sg - a texture member a download moves to copy_src re-applies the declaration at the next read",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    require_arrays(*ctx);

    auto const kernel = co_await shaders::sticky_arrays.look.acquire_pipeline(*ctx);
    auto const first = make_image(*ctx, 0.25f);
    auto const second = make_image(*ctx, 0.75f);
    sg::buffer<float> const easels[] = {make_easel(*ctx), make_easel(*ctx), make_easel(*ctx)};

    auto cmd = ctx->create_command_list();
    auto const gallery = ctx->transient.create_binding_group(
        *cmd, ctx->cached.acquire_binding_group_layout<shaders::gallery>(),
        shaders::gallery{.images = {first.as_texture_view(), second.as_texture_view()}});
    auto deltas = cc::vector<i64>();
    auto transitions = cc::vector<i64>();
    auto downloaded = sg::bytes_future();
    auto const declared = cmd->compute.declare_array_texture_access(
        "gallery.images", images_at(sg::texture_layout::shader_texture, sg::texture_layout::shader_texture));
    look(*cmd, *kernel, *gallery, easels[0]);
    downloaded = cmd->download.bytes_from_texture(first.raw());
    for (auto op = 3; op <= 4; ++op)
    {
        auto const tracked = tracks(*cmd);
        auto const before = recorded(*cmd, sg::stat::texture_transitions);
        look(*cmd, *kernel, *gallery, easels[op - 2]);
        deltas.push_back(tracks(*cmd) - tracked);
        transitions.push_back(recorded(*cmd, sg::stat::texture_transitions) - before);
    }
    auto const back = cmd->download.data_from_buffer(easels[2]);
    ctx->submit_command_list(cc::move(cmd));

    // Without layouts a download is a read, which leaves the declaration current.
    CHECK(deltas[0] == reapplied(*ctx));
    CHECK(deltas[1] == 0);
    if (has_layouts(*ctx) && ctx->metrics.stats().is_counted(sg::stat::texture_transitions))
    {
        CHECK(transitions[0] >= 1); // back out of copy_src
        CHECK(transitions[1] == 0);
    }
    auto const got = co_await back.data();
    REQUIRE(got.size() == isize(k_threads));
    for (auto i = 0; i < k_threads; ++i)
        CHECK(got[i] == (i % 2 == 0 ? 0.25f : 0.75f)).context(cc::format("thread {}", i));
    CHECK((co_await downloaded.bytes()).size() == 16);
}

ASYNC_INVOCABLE_TEST("sg - an op that first applies an array and writes a member through a scalar leaves it not "
                     "current",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    require_arrays(*ctx);
    auto const unchecked = portability_checks_off(*ctx);

    auto const marking = co_await shaders::sticky_arrays.gather_and_mark.acquire_pipeline(*ctx);
    auto const kernel = co_await shaders::sticky_arrays.gather_shelf.acquire_pipeline(*ctx);
    sg::buffer<i32> const sources[] = {make_source(*ctx, 0), make_source(*ctx, 1), make_source(*ctx, 2)};
    sg::buffer<i32> const trays[] = {make_tray(*ctx), make_tray(*ctx), make_tray(*ctx)};

    auto cmd = ctx->create_command_list();
    auto const shelf = shelf_of(*cmd, sources);
    auto deltas = cc::vector<i64>();
    auto barriers = cc::vector<i64>();
    auto const declared = cmd->compute.declare_array_buffer_access("shelf.sources", reads_of_three());

    // Op 1 writes source 1's upper half through the patch, tracked before the shelf's application in the same op.
    auto const tray_group
        = ctx->transient.create_binding_group(*cmd, ctx->cached.acquire_binding_group_layout<shaders::tray>(),
                                              shaders::tray{.merged = trays[0].as_readwrite_buffer()});
    auto const patch
        = ctx->transient.create_binding_group(*cmd, ctx->cached.acquire_binding_group_layout<shaders::patch>(),
                                              shaders::patch{.target = sources[1].as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*marking);
    cmd->compute.bind_group(0, *shelf);
    cmd->compute.bind_group(1, *tray_group);
    cmd->compute.bind_group(2, *patch);
    cmd->compute.dispatch_threads(k_threads);

    for (auto op = 2; op <= 3; ++op)
    {
        auto const tracked = tracks(*cmd);
        auto const barriers_before = recorded(*cmd, sg::stat::buffer_barriers);
        gather(*cmd, *kernel, *shelf, trays[op - 1]);
        deltas.push_back(tracks(*cmd) - tracked);
        barriers.push_back(recorded(*cmd, sg::stat::buffer_barriers) - barriers_before);
    }
    auto const back = cmd->download.data_from_buffer(trays[2]);
    ctx->submit_command_list(cc::move(cmd));

    // Op 2 re-applies, and so orders its read of source 1 after op 1's write; op 3 then skips.
    CHECK(deltas[0] == 3);
    CHECK(deltas[1] == 0);
    if (ctx->metrics.stats().is_counted(sg::stat::buffer_barriers))
        CHECK(barriers[0] >= 1);
    check_gathered(co_await back.data(), 0, "the gather after the marking op");
}

ASYNC_INVOCABLE_TEST("sg - two declarations holding one texture at different layouts each re-apply after the other",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    require_arrays(*ctx);

    auto const look_gallery = co_await shaders::sticky_arrays.look.acquire_pipeline(*ctx);
    auto const look_annex = co_await shaders::sticky_arrays.look_annex.acquire_pipeline(*ctx);
    auto const first = make_image(*ctx, 0.25f);
    auto const second = make_image(*ctx, 0.75f);
    sg::buffer<float> const easels[] = {make_easel(*ctx), make_easel(*ctx), make_easel(*ctx), make_easel(*ctx)};

    auto cmd = ctx->create_command_list();
    auto const gallery = ctx->transient.create_binding_group(
        *cmd, ctx->cached.acquire_binding_group_layout<shaders::gallery>(),
        shaders::gallery{.images = {first.as_texture_view(), second.as_texture_view()}});
    auto const annex = ctx->transient.create_binding_group(
        *cmd, ctx->cached.acquire_binding_group_layout<shaders::annex>(),
        shaders::annex{.images = {first.as_texture_view(), second.as_texture_view()}});
    auto deltas = cc::vector<i64>();
    // One bind point, two arrays of other names holding the same textures, each at its own layout.
    auto const a = cmd->compute.declare_array_texture_access(
        "gallery.images", images_at(sg::texture_layout::shader_texture, sg::texture_layout::shader_texture));
    auto const b = cmd->compute.declare_array_texture_access(
        "annex.images", images_at(sg::texture_layout::general, sg::texture_layout::general));

    auto const counted_look = [&](sg::compute_pipeline const& kernel, sg::binding_group const& images, int easel)
    {
        auto const tracked = tracks(*cmd);
        look(*cmd, kernel, images, easels[easel]);
        deltas.push_back(tracks(*cmd) - tracked);
    };
    counted_look(*look_gallery, *gallery, 0); // a applies
    counted_look(*look_annex, *annex, 1);     // b applies, moving both textures to general
    counted_look(*look_gallery, *gallery, 2); // a must move them back, so it applies again
    counted_look(*look_gallery, *gallery, 3); // and is current
    auto const back = cmd->download.data_from_buffer(easels[2]);
    ctx->submit_command_list(cc::move(cmd));

    CHECK(deltas[0] == 2);
    CHECK(deltas[1] == 2);
    CHECK(deltas[2] == reapplied(*ctx));
    CHECK(deltas[3] == 0);
    auto const got = co_await back.data();
    REQUIRE(got.size() == isize(k_threads));
    for (auto i = 0; i < k_threads; ++i)
        CHECK(got[i] == (i % 2 == 0 ? 0.25f : 0.75f)).context(cc::format("thread {}", i));
}

ASYNC_INVOCABLE_TEST("sg - a current array declaration applies after all when a later array of its op moves a member",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    require_arrays(*ctx);

    auto const look_gallery = co_await shaders::sticky_arrays.look.acquire_pipeline(*ctx);
    auto const look_both = co_await shaders::sticky_arrays.look_beside_annex.acquire_pipeline(*ctx);
    auto const first = make_image(*ctx, 0.25f);
    auto const second = make_image(*ctx, 0.75f);
    sg::buffer<float> const easels[] = {make_easel(*ctx), make_easel(*ctx)};

    auto cmd = ctx->create_command_list();
    auto const gallery = ctx->transient.create_binding_group(
        *cmd, ctx->cached.acquire_binding_group_layout<shaders::gallery>(),
        shaders::gallery{.images = {first.as_texture_view(), second.as_texture_view()}});
    auto const annex = ctx->transient.create_binding_group(
        *cmd, ctx->cached.acquire_binding_group_layout<shaders::annex>(),
        shaders::annex{.images = {first.as_texture_view(), second.as_texture_view()}});
    auto const easel_group
        = ctx->transient.create_binding_group(*cmd, ctx->cached.acquire_binding_group_layout<shaders::easel>(),
                                              shaders::easel{.reds = easels[1].as_readwrite_buffer()});
    auto const a = cmd->compute.declare_array_texture_access(
        "gallery.images", images_at(sg::texture_layout::shader_texture, sg::texture_layout::shader_texture));
    auto const b = cmd->compute.declare_array_texture_access(
        "annex.images", images_at(sg::texture_layout::general, sg::texture_layout::general));

    look(*cmd, *look_gallery, *gallery, easels[0]); // a applies, and is current
    auto const tracked = tracks(*cmd);
    cmd->compute.bind_pipeline(*look_both);
    cmd->compute.bind_group(0, *gallery);
    cmd->compute.bind_group(1, *annex);
    cmd->compute.bind_group(2, *easel_group);
    cmd->compute.dispatch_threads(k_threads);
    auto const delta = tracks(*cmd) - tracked;
    auto const back = cmd->download.data_from_buffer(easels[1]);
    ctx->submit_command_list(cc::move(cmd));

    // a is skipped as current, then b moves both textures to general.
    // Where that is a layout change, a applies after all, so the op's barrier serves both arrays.
    CHECK(delta == 2 + reapplied(*ctx));
    auto const got = co_await back.data();
    REQUIRE(got.size() == isize(k_threads));
    for (auto i = 0; i < k_threads; ++i)
        CHECK(got[i] == (i % 2 == 0 ? 0.25f : 0.75f)).context(cc::format("thread {}", i));
}

ASYNC_INVOCABLE_TEST("sg - groups alternating A, B, A at one array name re-apply where the other moved a layout",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    require_arrays(*ctx);

    auto const kernel = co_await shaders::sticky_arrays.look.acquire_pipeline(*ctx);
    auto const first = make_image(*ctx, 0.25f);
    auto const second = make_image(*ctx, 0.75f);
    sg::buffer<float> const easels[]
        = {make_easel(*ctx), make_easel(*ctx), make_easel(*ctx), make_easel(*ctx), make_easel(*ctx)};

    auto cmd = ctx->create_command_list();
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::gallery>();
    // The same two textures at swapped indices, so one declaration asks each for both layouts in turn.
    auto const group_a = ctx->transient.create_binding_group(
        *cmd, layout, shaders::gallery{.images = {first.as_texture_view(), second.as_texture_view()}});
    auto const group_b = ctx->transient.create_binding_group(
        *cmd, layout, shaders::gallery{.images = {second.as_texture_view(), first.as_texture_view()}});
    auto deltas = cc::vector<i64>();
    auto const declared = cmd->compute.declare_array_texture_access(
        "gallery.images", images_at(sg::texture_layout::shader_texture, sg::texture_layout::general));
    auto const counted_look = [&](sg::binding_group const& images, int easel)
    {
        auto const tracked = tracks(*cmd);
        look(*cmd, *kernel, images, easels[easel]);
        deltas.push_back(tracks(*cmd) - tracked);
    };
    counted_look(*group_a, 0); // A applies
    counted_look(*group_b, 1); // B is a group of its own, and moves both textures
    counted_look(*group_a, 2); // A is no longer current
    counted_look(*group_a, 3); // and now is
    counted_look(*group_b, 4); // B is not, since A moved them back
    auto const back_a = cmd->download.data_from_buffer(easels[3]);
    auto const back_b = cmd->download.data_from_buffer(easels[4]);
    ctx->submit_command_list(cc::move(cmd));

    CHECK(deltas[0] == 2);
    CHECK(deltas[1] == 2);
    CHECK(deltas[2] == reapplied(*ctx));
    CHECK(deltas[3] == 0);
    CHECK(deltas[4] == reapplied(*ctx));
    auto const got_a = co_await back_a.data();
    auto const got_b = co_await back_b.data();
    REQUIRE(got_a.size() == isize(k_threads));
    REQUIRE(got_b.size() == isize(k_threads));
    for (auto i = 0; i < k_threads; ++i)
    {
        CHECK(got_a[i] == (i % 2 == 0 ? 0.25f : 0.75f)).context(cc::format("A, thread {}", i));
        CHECK(got_b[i] == (i % 2 == 0 ? 0.75f : 0.25f)).context(cc::format("B, thread {}", i));
    }
}

ASYNC_INVOCABLE_TEST("sg - a compute declaration re-applies after a raster declaration moved its texture",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    require_arrays(*ctx);

    // The draw's declaration moves both images out of `general` inside the scope, which splits it on vulkan.
    // The warning comes once per context and cause, so this test sees it only when no earlier test on that context split a scope around a barrier.
    nx::allow_warnings("was closed and reopened around a barrier", "sg");

    auto const look_annex = co_await shaders::sticky_arrays.look_annex.acquire_pipeline(*ctx);
    auto const draw = co_await ctx->cached.acquire_raster_pipeline(shaders::sticky_arrays.gallery_in_fragment);
    auto const first = make_image(*ctx, 0.25f);
    auto const second = make_image(*ctx, 0.75f);
    auto const target
        = ctx->persistent.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                             .width = 1,
                                             .height = 1,
                                             .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});
    sg::buffer<float> const easels[] = {make_easel(*ctx), make_easel(*ctx), make_easel(*ctx)};

    auto cmd = ctx->create_command_list();
    auto const gallery = ctx->transient.create_binding_group(
        *cmd, ctx->cached.acquire_binding_group_layout<shaders::gallery>(),
        shaders::gallery{.images = {first.as_texture_view(), second.as_texture_view()}});
    auto const annex = ctx->transient.create_binding_group(
        *cmd, ctx->cached.acquire_binding_group_layout<shaders::annex>(),
        shaders::annex{.images = {first.as_texture_view(), second.as_texture_view()}});
    auto deltas = cc::vector<i64>();
    auto const computed = cmd->compute.declare_array_texture_access(
        "annex.images", images_at(sg::texture_layout::general, sg::texture_layout::general));
    look(*cmd, *look_annex, *annex, easels[0]);
    {
        auto pass
            = cmd->raster.render_to({.color_targets = {target.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 0))},
                                     .target_set = shaders::shelf_target::name});
        auto const drawn = pass.declare_array_texture_access(
            "gallery.images", images_at(sg::texture_layout::shader_texture, sg::texture_layout::shader_texture));
        pass.bind_pipeline(*draw);
        pass.bind_group(0, *gallery);
        pass.draw({.vertex_range = {.offset = 0, .size = 3}});
    }
    for (auto op = 1; op <= 2; ++op)
    {
        auto const tracked = tracks(*cmd);
        look(*cmd, *look_annex, *annex, easels[op]);
        deltas.push_back(tracks(*cmd) - tracked);
    }
    auto const back = cmd->download.data_from_buffer(easels[1]);
    auto const drawn_back = cmd->download.bytes_from_texture(target.raw());
    ctx->submit_command_list(cc::move(cmd));

    CHECK(deltas[0] == reapplied(*ctx)); // the draw moved both textures to shader_texture
    CHECK(deltas[1] == 0);
    auto const got = co_await back.data();
    REQUIRE(got.size() == isize(k_threads));
    for (auto i = 0; i < k_threads; ++i)
        CHECK(got[i] == (i % 2 == 0 ? 0.25f : 0.75f)).context(cc::format("thread {}", i));
    auto const pixel = co_await drawn_back.bytes();
    REQUIRE(pixel.size() == 16);
    auto red = 0.0f;
    cc::memcpy(&red, pixel.data(), 4);
    CHECK(red == 0.25f);
}

ASYNC_INVOCABLE_TEST("sg - a compute declaration alive across a rendering scope that writes a member re-applies",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    require_arrays(*ctx);

    auto const kernel = co_await shaders::sticky_arrays.look.acquire_pipeline(*ctx);
    auto const first = make_image(*ctx, 0.25f, sg::texture_usage::render_target);
    auto const second = make_image(*ctx, 0.75f);
    sg::buffer<float> const easels[] = {make_easel(*ctx), make_easel(*ctx), make_easel(*ctx)};

    auto cmd = ctx->create_command_list();
    auto const gallery = ctx->transient.create_binding_group(
        *cmd, ctx->cached.acquire_binding_group_layout<shaders::gallery>(),
        shaders::gallery{.images = {first.as_texture_view(), second.as_texture_view()}});
    auto deltas = cc::vector<i64>();
    auto const declared = cmd->compute.declare_array_texture_access(
        "gallery.images", images_at(sg::texture_layout::shader_texture, sg::texture_layout::shader_texture));
    look(*cmd, *kernel, *gallery, easels[0]);
    {
        // The scope clears the first image: a render-target write to an array member.
        auto const pass
            = cmd->raster.render_to({.color_targets = {first.as_render_target_view().cleared(tg::vec4f(0.5f, 0, 0, 1))}});
    }
    for (auto op = 1; op <= 2; ++op)
    {
        auto const tracked = tracks(*cmd);
        look(*cmd, *kernel, *gallery, easels[op]);
        deltas.push_back(tracks(*cmd) - tracked);
    }
    auto const back = cmd->download.data_from_buffer(easels[1]);
    ctx->submit_command_list(cc::move(cmd));

    CHECK(deltas[0] == 2);
    CHECK(deltas[1] == 0);
    auto const got = co_await back.data();
    REQUIRE(got.size() == isize(k_threads));
    for (auto i = 0; i < k_threads; ++i)
        CHECK(got[i] == (i % 2 == 0 ? 0.5f : 0.75f)).context(cc::format("thread {}", i));
}

ASYNC_INVOCABLE_TEST("sg - a one-op declaration of another array leaves a sticky one current",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    require_arrays(*ctx);

    auto const kernel = co_await shaders::sticky_arrays.gather_shelf.acquire_pipeline(*ctx);
    auto const looker = co_await shaders::sticky_arrays.look.acquire_pipeline(*ctx);
    sg::buffer<i32> const sources[] = {make_source(*ctx, 0), make_source(*ctx, 1), make_source(*ctx, 2)};
    sg::buffer<i32> const trays[] = {make_tray(*ctx), make_tray(*ctx)};
    auto const first = make_image(*ctx, 0.25f);
    auto const second = make_image(*ctx, 0.75f);
    auto const easel = make_easel(*ctx);

    auto cmd = ctx->create_command_list();
    auto const shelf = shelf_of(*cmd, sources);
    auto const gallery = ctx->transient.create_binding_group(
        *cmd, ctx->cached.acquire_binding_group_layout<shaders::gallery>(),
        shaders::gallery{.images = {first.as_texture_view(), second.as_texture_view()}});
    auto delta = i64(-1);
    auto const sticky = cmd->compute.declare_array_buffer_access("shelf.sources", reads_of_three());
    gather(*cmd, *kernel, *shelf, trays[0]);
    {
        auto const once = cmd->compute.declare_array_texture_access(
            "gallery.images", images_at(sg::texture_layout::shader_texture, sg::texture_layout::shader_texture));
        look(*cmd, *looker, *gallery, easel);
    }
    auto const tracked = tracks(*cmd);
    gather(*cmd, *kernel, *shelf, trays[1]);
    delta = tracks(*cmd) - tracked;
    auto const back = cmd->download.data_from_buffer(trays[1]);
    ctx->submit_command_list(cc::move(cmd));

    CHECK(delta == 0);
    check_gathered(co_await back.data(), 0, "the gather after the other declaration");
}

ASYNC_INVOCABLE_TEST("sg - two raster pipelines touching an array at different stages each apply once, then skip",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    require_arrays(*ctx);

    auto const fragment = co_await ctx->cached.acquire_raster_pipeline(shaders::sticky_arrays.shelf_in_fragment);
    auto const both = co_await ctx->cached.acquire_raster_pipeline(shaders::sticky_arrays.shelf_in_both);
    sg::buffer<i32> const sources[] = {make_source(*ctx, 0), make_source(*ctx, 1), make_source(*ctx, 2)};
    auto const target = ctx->persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba32_float, .width = 1, .height = 1, .usage = sg::texture_usage::render_target});

    auto cmd = ctx->create_command_list();
    auto const shelf = shelf_of(*cmd, sources);
    auto deltas = cc::vector<i64>();
    {
        auto pass
            = cmd->raster.render_to({.color_targets = {target.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 0))},
                                     .target_set = shaders::shelf_target::name});
        auto const declared = pass.declare_array_buffer_access("shelf.sources", reads_of_three());
        for (auto const* pipeline : {fragment.get(), both.get(), fragment.get(), both.get()})
        {
            auto const tracked = tracks(*cmd);
            pass.bind_pipeline(*pipeline);
            pass.bind_group(0, *shelf);
            pass.draw({.vertex_range = {.offset = 0, .size = 3}});
            deltas.push_back(tracks(*cmd) - tracked);
        }
    }
    ctx->submit_command_list(cc::move(cmd));

    // The second pipeline reads at the vertex stage too, outside what the first applied; after that both are covered.
    CHECK(deltas[0] == 3);
    CHECK(deltas[1] == 3);
    CHECK(deltas[2] == 0);
    CHECK(deltas[3] == 0);
    co_await ctx->idle_completion();
}

ASYNC_INVOCABLE_TEST("sg - a dispatch and a trace on one bind point each apply an array declaration once, then skip",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    require_arrays(*ctx);
    if (!ctx->supports(sg::feature::raytracing_pipeline))
        SKIP("this device has no ray-tracing pipelines");

    using traced_t = shaders::sticky_arrays_traced_traced_gather_t;
    auto const kernel = co_await shaders::sticky_arrays_traced.gather_rack.acquire_pipeline(*ctx);
    auto const desc = co_await shaders::sticky_arrays_traced.traced_gather.description(*ctx);
    auto const pipeline = co_await ctx->cached.acquire_raytracing_pipeline(desc);
    REQUIRE(pipeline != nullptr);
    auto const table = ctx->uncached.create_raytracing_shader_table(traced_t::table_description(pipeline));
    REQUIRE(table != nullptr);
    sg::buffer<i32> const sources[] = {make_source(*ctx, 0), make_source(*ctx, 1), make_source(*ctx, 2)};
    sg::buffer<i32> const trays[] = {make_tray(*ctx), make_tray(*ctx), make_tray(*ctx), make_tray(*ctx)};

    auto cmd = ctx->create_command_list();
    auto const rack = ctx->transient.create_binding_group(
        *cmd, ctx->cached.acquire_binding_group_layout<shaders::rack>(),
        shaders::rack{.sources = {sources[0].as_readonly_buffer(), sources[1].as_readonly_buffer(),
                                  sources[2].as_readonly_buffer()}});
    // Made on the compute scope, and in force for the traces too: one bind point.
    auto const declared = cmd->compute.declare_array_buffer_access("rack.sources", reads_of_three());
    auto deltas = cc::vector<i64>();
    for (auto op = 0; op < 4; ++op)
    {
        auto const bin
            = ctx->transient.create_binding_group(*cmd, ctx->cached.acquire_binding_group_layout<shaders::bin>(),
                                                  shaders::bin{.merged = trays[op].as_readwrite_buffer()});
        auto const tracked = tracks(*cmd);
        if (op % 2 == 0)
        {
            cmd->compute.bind_pipeline(*kernel);
            cmd->compute.bind_group(0, *rack);
            cmd->compute.bind_group(1, *bin);
            cmd->compute.dispatch_threads(k_threads);
        }
        else
        {
            cmd->raytracing.bind_pipeline(*pipeline);
            cmd->raytracing.bind_group(0, *rack);
            cmd->raytracing.bind_group(1, *bin);
            cmd->raytracing.dispatch_rays(*table, sg::raygen_index(0), k_threads);
        }
        deltas.push_back(tracks(*cmd) - tracked);
    }
    auto const dispatched_back = cmd->download.data_from_buffer(trays[2]);
    auto const traced_back = cmd->download.data_from_buffer(trays[3]);
    ctx->submit_command_list(cc::move(cmd));

    // The trace reads at the raytracing stage, outside what the dispatch applied; after that the union covers both.
    CHECK(deltas[0] == 3);
    CHECK(deltas[1] == 3);
    CHECK(deltas[2] == 0);
    CHECK(deltas[3] == 0);
    check_gathered(co_await dispatched_back.data(), 0, "the second dispatch");
    check_gathered(co_await traced_back.data(), 0, "the second trace");
}

ASYNC_INVOCABLE_TEST("sg - a withdrawn array declaration leaves its array undeclared, covered and logged",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    require_arrays(*ctx);

    auto const kernel = co_await shaders::sticky_arrays.gather_shelf.acquire_pipeline(*ctx);
    sg::buffer<i32> const sources[] = {make_source(*ctx, 0), make_source(*ctx, 1), make_source(*ctx, 2)};
    sg::buffer<i32> const trays[] = {make_tray(*ctx), make_tray(*ctx)};

    nx::expect_error("declared no access for a bound array", nx::exactly(1));
    auto cmd = ctx->create_command_list();
    auto const shelf = shelf_of(*cmd, sources);
    auto declared = cmd->compute.declare_array_buffer_access("shelf.sources", reads_of_three());
    gather(*cmd, *kernel, *shelf, trays[0]);
    declared.withdraw();
    auto const tracked = tracks(*cmd);
    gather(*cmd, *kernel, *shelf, trays[1]);
    CHECK(tracks(*cmd) - tracked == 3); // every element, covered
    auto const back = cmd->download.data_from_buffer(trays[1]);
    ctx->submit_command_list(cc::move(cmd));

    check_gathered(co_await back.data(), 0, "the covered gather");
}

ASYNC_INVOCABLE_TEST("sg - an array declaration that never met its array asserts at withdraw, and a drop ends one "
                     "silently",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    require_arrays(*ctx);

    auto const kernel = co_await shaders::sticky_arrays.gather_shelf.acquire_pipeline(*ctx);
    sg::buffer<i32> const sources[] = {make_source(*ctx, 0), make_source(*ctx, 1), make_source(*ctx, 2)};
    auto const tray = make_tray(*ctx);

    auto cmd = ctx->create_command_list();
    auto const shelf = shelf_of(*cmd, sources);
    auto const declared = cmd->compute.declare_array_buffer_access("shelf.sources", reads_of_three());
    // A misspelled name is legal while it lives — a dormant declaration is — and caught where it ends.
    auto misspelled = cmd->compute.declare_array_buffer_access("shelf.source", {});
    gather(*cmd, *kernel, *shelf, tray);
    CHECK_ASSERTS(misspelled.withdraw());

    // A list dropped with declarations alive ends them, and their values find nothing to end afterwards.
    auto dropped = cmd->compute.declare_array_buffer_access("shelf.source", {});
    ctx->drop_command_list(cc::move(cmd));
    dropped.withdraw();
    co_await ctx->idle_completion();
}

ASYNC_INVOCABLE_TEST("sg - a raster array declaration ends with its rendering scope, and its value then ends nothing",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    require_arrays(*ctx);

    auto const draw = co_await ctx->cached.acquire_raster_pipeline(shaders::sticky_arrays.shelf_in_both);
    sg::buffer<i32> const sources[] = {make_source(*ctx, 0), make_source(*ctx, 1), make_source(*ctx, 2)};
    auto const target = ctx->persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba32_float, .width = 1, .height = 1, .usage = sg::texture_usage::render_target});
    auto const color = target.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 0));

    nx::expect_error("declared no access for a bound array", nx::exactly(1));
    auto cmd = ctx->create_command_list();
    auto const shelf = shelf_of(*cmd, sources);
    auto outlived = sg::array_access_declaration();
    {
        auto pass = cmd->raster.render_to({.color_targets = {color}, .target_set = shaders::shelf_target::name});
        outlived = pass.declare_array_buffer_access("shelf.sources", reads_of_three());
        pass.bind_pipeline(*draw);
        pass.bind_group(0, *shelf);
        pass.draw({.vertex_range = {.offset = 0, .size = 3}});
    }
    auto delta = i64(0);
    {
        // Undeclared now: the draw covers the shelf, and logs that it did.
        auto pass = cmd->raster.render_to({.color_targets = {color}, .target_set = shaders::shelf_target::name});
        pass.bind_pipeline(*draw);
        pass.bind_group(0, *shelf);
        auto const tracked = tracks(*cmd);
        pass.draw({.vertex_range = {.offset = 0, .size = 3}});
        delta = tracks(*cmd) - tracked;
    }
    outlived.withdraw();
    ctx->submit_command_list(cc::move(cmd));

    CHECK(delta == 3);
    co_await ctx->idle_completion();
}

ASYNC_INVOCABLE_TEST("sg - submitting a list ends a live array declaration silently, and the value is then inert",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    require_arrays(*ctx);

    auto const kernel = co_await shaders::sticky_arrays.gather_shelf.acquire_pipeline(*ctx);
    sg::buffer<i32> const sources[] = {make_source(*ctx, 0), make_source(*ctx, 1), make_source(*ctx, 2)};
    auto const tray = make_tray(*ctx);

    auto cmd = ctx->create_command_list();
    auto const shelf = shelf_of(*cmd, sources);
    auto declared = cmd->compute.declare_array_buffer_access("shelf.sources", reads_of_three());
    // Never met, so a withdraw would assert: the submit ending it must not run that check either.
    auto misspelled = cmd->compute.declare_array_buffer_access("shelf.source", {});
    gather(*cmd, *kernel, *shelf, tray);
    auto const back = cmd->download.data_from_buffer(tray);
    ctx->submit_command_list(cc::move(cmd));

    // The list is gone, so the values find nothing to end, withdrawn or destroyed.
    declared.withdraw();
    misspelled.withdraw();
    check_gathered(co_await back.data(), 0, "the gather submitted with its declaration alive");
}

ASYNC_INVOCABLE_TEST("sg - a raster array declaration that never met its array asserts when its rendering scope "
                     "closes, "
                     "where a draw bound an array of its family",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    require_arrays(*ctx);

    auto const draw = co_await ctx->cached.acquire_raster_pipeline(shaders::sticky_arrays.shelf_in_both);
    sg::buffer<i32> const sources[] = {make_source(*ctx, 0), make_source(*ctx, 1), make_source(*ctx, 2)};
    auto const target = ctx->persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba32_float, .width = 1, .height = 1, .usage = sg::texture_usage::render_target});
    auto const color = target.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 0));

    auto cmd = ctx->create_command_list();
    auto const shelf = shelf_of(*cmd, sources);
    auto outlived = sg::array_access_declaration();

    // A scope that draws nothing never shows its declarations a buffer array, so its close ends them silently.
    cmd->raster.manual.begin_rendering({.color_targets = {color}, .target_set = shaders::shelf_target::name});
    outlived = cmd->raster.manual.declare_array_buffer_access("shelf.source", {});
    cmd->raster.manual.end_rendering();

    // One whose draw binds the shelf, a buffer array of another name, ends never met: the close asserts.
    cmd->raster.manual.begin_rendering({.color_targets = {color}, .target_set = shaders::shelf_target::name});
    auto const declared = cmd->raster.manual.declare_array_buffer_access("shelf.sources", reads_of_three());
    outlived = cmd->raster.manual.declare_array_buffer_access("shelf.source", {});
    cmd->raster.manual.bind_pipeline(*draw);
    cmd->raster.manual.bind_group(0, *shelf);
    cmd->raster.manual.draw({.vertex_range = {.offset = 0, .size = 3}});
#if CC_ASSERT_ENABLED
    CHECK_ASSERTS(cmd->raster.manual.end_rendering());
#else
    cmd->raster.manual.end_rendering();
#endif
    // Ended by the close, which left the list whole: the values end nothing, and the list drops.
    outlived.withdraw();
    ctx->drop_command_list(cc::move(cmd));
    co_await ctx->idle_completion();
}

ASYNC_INVOCABLE_TEST("sg - unwinding past an array declaration that never met its array does not assert",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    require_arrays(*ctx);

    auto const kernel = co_await shaders::sticky_arrays.gather_shelf.acquire_pipeline(*ctx);
    auto const draw = co_await ctx->cached.acquire_raster_pipeline(shaders::sticky_arrays.shelf_in_both);
    sg::buffer<i32> const sources[] = {make_source(*ctx, 0), make_source(*ctx, 1), make_source(*ctx, 2)};
    auto const tray = make_tray(*ctx);
    auto const target = ctx->persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba32_float, .width = 1, .height = 1, .usage = sg::texture_usage::render_target});
    auto const color = target.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 0));

    // Each misspelled declaration sees an op bind the shelf, a buffer array of another name, so only the unwinding spares it.
    auto cmd = ctx->create_command_list();
    auto const shelf = shelf_of(*cmd, sources);
    // A declaration is made after its list, so unwinding destroys it while the list lives.
    auto const unwind_past_value = [&]
    {
        auto const declared = cmd->compute.declare_array_buffer_access("shelf.sources", reads_of_three());
        auto const misspelled = cmd->compute.declare_array_buffer_access("shelf.source", {});
        gather(*cmd, *kernel, *shelf, tray);
        throw unwound{};
    };
    // A raster one held past its scope is ended by the close the unwinding runs.
    auto outlived = sg::array_access_declaration();
    auto const unwind_past_scope = [&]
    {
        auto pass = cmd->raster.render_to({.color_targets = {color}, .target_set = shaders::shelf_target::name});
        auto const declared = pass.declare_array_buffer_access("shelf.sources", reads_of_three());
        outlived = pass.declare_array_buffer_access("shelf.source", {});
        pass.bind_pipeline(*draw);
        pass.bind_group(0, *shelf);
        pass.draw({.vertex_range = {.offset = 0, .size = 3}});
        throw unwound{};
    };
    CHECK_THROWS_AS(unwind_past_value(), unwound);
    CHECK_THROWS_AS(unwind_past_scope(), unwound);
    outlived.withdraw();
    ctx->drop_command_list(cc::move(cmd));
    co_await ctx->idle_completion();
}

ASYNC_INVOCABLE_TEST("sg - an array declaration whose bind point saw no op ends without asserting",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto const target = ctx->persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba8_unorm, .width = 1, .height = 1, .usage = sg::texture_usage::render_target});

    // A frame with nothing to dispatch or draw: dormant all its life, withdrawn or ended by its rendering scope's close.
    // On every backend, webgpu included, where no declaration ever meets an array.
    auto cmd = ctx->create_command_list();
    auto computed = cmd->compute.declare_array_buffer_access("shelf.sources", {});
    auto drawn = sg::array_access_declaration();
    {
        auto pass
            = cmd->raster.render_to({.color_targets = {target.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 0))}});
        drawn = pass.declare_array_texture_access("gallery.images", {});
    }
    computed.withdraw();
    ctx->drop_command_list(cc::move(cmd));
    co_await ctx->idle_completion();
}

ASYNC_INVOCABLE_TEST("sg - the first-meeting check of an array declaration compiles out with the other asserts",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    require_arrays(*ctx);

    auto const kernel = co_await shaders::sticky_arrays.gather_shelf.acquire_pipeline(*ctx);
    sg::buffer<i32> const sources[] = {make_source(*ctx, 0), make_source(*ctx, 1), make_source(*ctx, 2)};
    auto const tray = make_tray(*ctx);

    // The range and vacancy check runs once per group the declarations meet, which is what makes it cheap enough to
    // keep — and it is a CC_ASSERT like the rest, so a release preset neither runs it nor records the op that would trip it.
    sg::array_buffer_access const out_of_range[] = {{.index = 3, .access = sg::access_flag::shader_read}};
    auto cmd = ctx->create_command_list();
    auto const shelf = shelf_of(*cmd, sources);
    auto const bad = cmd->compute.declare_array_buffer_access("shelf.sources", out_of_range);
    CHECK_ASSERTS(gather(*cmd, *kernel, *shelf, tray));
    // Ended silently by the drop, met or not.
    ctx->drop_command_list(cc::move(cmd));
    co_await ctx->idle_completion();
}

ASYNC_INVOCABLE_TEST("sg - an async upload into an applied array member before the submit still makes the list wait",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    require_arrays(*ctx);
    // The transfer waits are folded at submit on these two; the claim is theirs.
    if (ctx->backend() != sg::backend_kind::dx12 && ctx->backend() != sg::backend_kind::vulkan)
        SKIP("this backend's transfer waits are not the subject here");

    auto const kernel = co_await shaders::sticky_arrays.gather_shelf.acquire_pipeline(*ctx);
    sg::buffer<i32> const sources[] = {make_source(*ctx, 0), make_source(*ctx, 1), make_source(*ctx, 2)};
    auto const tray = make_tray(*ctx);
    co_await ctx->idle_completion();

    auto cmd = ctx->create_command_list();
    auto const shelf = shelf_of(*cmd, sources);
    auto const declared = cmd->compute.declare_array_buffer_access("shelf.sources", reads_of_three());
    gather(*cmd, *kernel, *shelf, tray);

    // Enqueued after the application recorded, before the submit: the list must still run after it lands.
    auto words = cc::vector<i32>();
    for (auto i = 0; i < k_words; ++i)
        words.push_back(1000 + i + 5000);
    ctx->upload.data_to_buffer(sources[1], cc::make_pinned_data(cc::move(words)));

    auto const back = cmd->download.data_from_buffer(tray);
    ctx->submit_command_list(cc::move(cmd));
    check_gathered(co_await back.data(), 5000, "the gather after the async upload");
}
