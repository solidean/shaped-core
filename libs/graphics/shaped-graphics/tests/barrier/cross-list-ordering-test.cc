#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/raw_buffer.hh>
#include <shaped-graphics/types.hh>

using namespace cc::primitive_defines;


// A write recorded in one command list, read by the next, with an off-frame transfer running alongside.
//
// The alongside part is the whole test.
// A backend is free to order two command buffers however it likes, and Metal's cheapest mechanism for it — the
// queue-scoped barrier pair an encoder opens and closes with — stops working once a queue wait sits between the two
// commits, which is exactly what a pending async transfer puts there.
// The hazard is invisible without one: the same two lists pass on their own, on every backend, for as long as the
// queue happens to drain between them.
//
// See libs/graphics/shaped-graphics/docs/concepts/barriers.md.

namespace
{
constexpr int k_element_count = 1024;
constexpr int k_half = k_element_count / 2;
constexpr int k_iterations = 100;

sg::raw_buffer_handle make_buffer(sg::context_handle const& ctx)
{
    return ctx->persistent.create_raw_buffer(k_element_count * isize(sizeof(u32)),
                                             sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
}

/// Records the write-then-read pair `k_iterations` times without waiting in between, so the queue stays deep — which
/// is when a missing dependency actually shows.
/// `aux` is touched by both lists purely so each submit has an in-flight async transfer to order against; its contents
/// are never checked.
void record_write_then_read(sg::context_handle const& ctx,
                            sg::raw_buffer_handle const& buffer,
                            sg::raw_buffer_handle const& aux,
                            cc::vector<sg::data_future<u32>>& out_downloads,
                            cc::vector<cc::vector<u32>>& out_expected)
{
    cc::random rng;
    auto data = cc::vector<u32>::create_uninitialized(k_element_count);

    for (auto iteration = 0; iteration < k_iterations; ++iteration)
    {
        for (auto& d : data)
            d = rng.next_u32();

        auto aux_data = cc::vector<u32>::create_uninitialized(k_element_count);
        for (auto& d : aux_data)
            d = rng.next_u32();
        ctx->upload.data_to_buffer<u32>(aux, cc::make_pinned_data(cc::move(aux_data)), 0);

        // First list: fill the buffer.
        auto writer = ctx->create_command_list();
        writer->upload.data_to_buffer(buffer, data);
        writer->copy.buffer_data_region<u32>({.src = aux, .dst = aux, .count = 8, .src_offset = 0, .dst_offset = 16});
        ctx->submit_command_list(cc::move(writer));

        // Second list: read what the first wrote, fold it down, and read the result back.
        auto reader = ctx->create_command_list();
        reader->copy.buffer_data_region<u32>(
            {.src = buffer, .dst = buffer, .count = k_half, .src_offset = k_half, .dst_offset = 0});
        reader->copy.buffer_data_region<u32>({.src = aux, .dst = aux, .count = 8, .src_offset = 0, .dst_offset = 32});
        out_downloads.push_back(reader->download.data_from_buffer<u32>(buffer, 0, k_half));
        ctx->submit_command_list(cc::move(reader));

        out_expected.push_back(
            cc::vector<u32>::create_copy_of(cc::span<u32>(data).subspan({.start = k_half, .end = k_element_count})));
    }
}

void check_downloads(cc::span<sg::data_future<u32> const> downloads, cc::span<cc::vector<u32> const> expected)
{
    for (auto iteration = 0; iteration < isize(downloads.size()); ++iteration)
    {
        auto const downloaded = downloads[iteration].try_get_data().value();
        REQUIRE(downloaded.size() == isize(k_half));
        for (auto i = 0; i < k_half; ++i)
            CHECK(expected[iteration][i] == downloaded[i]);
    }
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - cross list ordering under a queue wait", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto downloads = cc::vector<sg::data_future<u32>>();
    auto expected = cc::vector<cc::vector<u32>>();
    record_write_then_read(ctx, make_buffer(ctx), make_buffer(ctx), downloads, expected);

    co_await ctx->idle_completion();
    check_downloads(downloads, expected);
}
