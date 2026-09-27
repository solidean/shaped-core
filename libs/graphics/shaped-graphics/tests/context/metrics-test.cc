#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/metrics.hh>
#include <shaped-graphics/resource/raw_buffer.hh>

using namespace cc::primitive_defines;

// What ctx.metrics.stats() counts, pinned from the outside on every backend.
// Barrier counts are pinned where they matter — beside the footprint that makes them zero, in footprint-barrier-test.cc.

namespace
{
cc::pinned_data<byte const> zeros(isize n)
{
    return cc::make_pinned_data(cc::vector<byte>::create_defaulted(n));
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - a list's counts join the stats when it submits, and a dropped list's never do",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const buf = ctx->persistent.create_raw_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    auto const data = cc::vector<byte>::create_defaulted(64);

    auto const before = ctx->metrics.stats();

    auto dropped = ctx->create_command_list();
    dropped->upload.bytes_to_buffer(buf, data, 0);
    ctx->drop_command_list(cc::move(dropped));
    CHECK((ctx->metrics.stats() - before)[sg::stat::bytes_uploaded_inline] == 0);

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_buffer(buf, data, 0);
    auto future = cmd->download.bytes_from_buffer(buf, 0, 32);

    // Recorded but not yet submitted: nothing is counted until the submit, which is what makes a dropped list free.
    CHECK((ctx->metrics.stats() - before)[sg::stat::bytes_uploaded_inline] == 0);
    ctx->submit_command_list(cc::move(cmd));

    auto const d = ctx->metrics.stats() - before;
    CHECK(d[sg::stat::command_lists_submitted] == 1);
    CHECK(d[sg::stat::bytes_uploaded_inline] == 64);
    CHECK(d[sg::stat::bytes_downloaded_inline] == 32);
    CHECK(d[sg::stat::draws] == 0);
    (void)co_await future.bytes();
}

ASYNC_INVOCABLE_TEST("sg - transfer bytes count per path, when the transfer is enqueued", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const buf = ctx->persistent.create_raw_buffer(1024, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    auto const before = ctx->metrics.stats();

    ctx->upload.bytes_to_buffer(buf, zeros(128));
    auto streamed = ctx->stream.bytes_to_buffer(buf, zeros(256), 256);
    auto read = ctx->download.bytes_from_buffer(buf, 0, 16);

    // Counted at the call, so this reading does not wait on any copy to land.
    auto const d = ctx->metrics.stats() - before;
    CHECK(d[sg::stat::bytes_uploaded_async] == 128);
    CHECK(d[sg::stat::bytes_uploaded_stream] == 256);
    CHECK(d[sg::stat::bytes_downloaded_async] == 16);
    CHECK(d[sg::stat::bytes_uploaded_inline] == 0);

    (void)co_await read.bytes();
    REQUIRE((co_await cc::async_as_result(streamed.completion())).has_value());
}

ASYNC_INVOCABLE_TEST("sg - advance_epoch counts itself, and creation is counted where it is asked for",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const before = ctx->metrics.stats();
    auto const buf = ctx->persistent.create_raw_buffer(64, sg::buffer_usage::copy_dst);
    ctx->advance_epoch();

    auto const d = ctx->metrics.stats() - before;
    CHECK(d[sg::stat::buffers_created] == 1);
    CHECK(d[sg::stat::epochs_advanced] == 1);
    co_return;
}

ASYNC_INVOCABLE_TEST("sg - a backend says which stats it cannot count", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const s = ctx->metrics.stats();

    // Every backend sees its own submits and transfers.
    CHECK(s.is_counted(sg::stat::command_lists_submitted));
    CHECK(s.is_counted(sg::stat::bytes_uploaded_inline));

    // WebGPU tracks usage itself, so a zero there would be a barrier count nobody measured.
    if (ctx->backend() == sg::backend_kind::webgpu)
        CHECK(!s.is_counted(sg::stat::buffer_barriers));
    // Metal's barriers name stages and never a resource.
    if (ctx->backend() == sg::backend_kind::metal)
    {
        CHECK(!s.is_counted(sg::stat::buffer_barriers));
        CHECK(!s.is_counted(sg::stat::texture_barriers));
        CHECK(s.is_counted(sg::stat::global_barriers));
    }
    if (ctx->backend() == sg::backend_kind::dx12 || ctx->backend() == sg::backend_kind::vulkan)
        CHECK(s.counted() == sg::all_stats);
    co_return;
}
