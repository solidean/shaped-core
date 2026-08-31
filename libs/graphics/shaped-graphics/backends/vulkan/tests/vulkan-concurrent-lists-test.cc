#include "vulkan-test-common.hh"

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/all.hh>

using namespace cc::primitive_defines;

// Whether two command lists recorded CONCURRENTLY against one resource end up correctly ordered once both submit.
//
// The tracker seeds a list's private state from the between-lists state at its first touch, so a list that opens
// before another one writes sees no writer in flight and takes the no-barrier freebie.
// Submission order then puts the write first and the read second, with nothing between them.
//
// dx12 is safe by construction: ExecuteCommandLists guarantees the first workload finishes before the second starts,
// and a buffer additionally decays to COMMON at that point.
// Vulkan makes no such promise — batches begin in submission order and may overlap — so the dependency has to come
// from a barrier, and the freebie is the case where none is recorded.
//
// A byte comparison proves nothing here: hardware will usually serialize anyway, so a green run is not evidence.
// SYNCHRONIZATION VALIDATION is the oracle, and make_context turns it on; the listener fails the test on any message
// it raises.

namespace
{
constexpr int k_extent = 16;
constexpr isize k_texture_bytes = isize(k_extent) * k_extent * 4;
constexpr isize k_buffer_bytes = 256;

cc::vector<byte> pattern(isize count, int salt)
{
    cc::vector<byte> out;
    out.reserve(count);
    for (isize i = 0; i < count; ++i)
        out.push_back(byte((int(i) * 7 + salt) & 0xFF));
    return out;
}

bool matches(cc::span<byte const> bytes, isize count, int salt)
{
    if (bytes.size() != count)
        return false;
    for (isize i = 0; i < count; ++i)
        if (bytes[i] != byte((int(i) * 7 + salt) & 0xFF))
            return false;
    return true;
}
} // namespace

TEST("sg vulkan - a buffer written by one concurrently recorded list and read by the next")
{
    auto const ctx = sg::backend::vulkan::test::make_context();
    if (ctx == nullptr)
        SKIP("no vulkan device");

    auto const buffer
        = ctx->persistent.create_raw_buffer(k_buffer_bytes, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buffer != nullptr);

    auto const bytes = pattern(k_buffer_bytes, 41);

    // Both open before either records, which is what makes the reader's state predate the writer's declares.
    auto writer = ctx->create_command_list();
    auto reader = ctx->create_command_list();
    REQUIRE(writer != nullptr);
    REQUIRE(reader != nullptr);

    writer->upload.bytes_to_buffer(buffer, cc::span<byte const>(bytes));
    auto future = reader->download.bytes_from_buffer(buffer, 0, k_buffer_bytes);

    ctx->submit_command_list(cc::move(writer));
    ctx->submit_command_list(cc::move(reader));

    auto const read_back = ctx->wait_for(future);
    REQUIRE(read_back.has_value());
    CHECK(matches(read_back.value(), k_buffer_bytes, 41));

    ctx->advance_epoch_and_wait_for_idle();
}

TEST("sg vulkan - a texture written by one concurrently recorded list and read by the next")
{
    auto const ctx = sg::backend::vulkan::test::make_context();
    if (ctx == nullptr)
        SKIP("no vulkan device");

    // Sampled as well as copyable, so the layout the lists rest in is a specific one rather than a transfer layout.
    auto const tex = ctx->persistent.create_raw_texture(
        {.format = sg::pixel_format::rgba8_unorm,
         .dimension = sg::texture_dimension::d2,
         .width = k_extent,
         .height = k_extent,
         .usage = sg::texture_usage::copy_src | sg::texture_usage::copy_dst | sg::texture_usage::readonly_texture});
    REQUIRE(tex != nullptr);

    auto const bytes = pattern(k_texture_bytes, 17);

    auto writer = ctx->create_command_list();
    auto reader = ctx->create_command_list();
    REQUIRE(writer != nullptr);
    REQUIRE(reader != nullptr);

    writer->upload.bytes_to_texture(tex, cc::span<byte const>(bytes));
    auto future = reader->download.bytes_from_texture(tex);

    ctx->submit_command_list(cc::move(writer));
    ctx->submit_command_list(cc::move(reader));

    auto const read_back = ctx->wait_for(future);
    REQUIRE(read_back.has_value());
    CHECK(matches(read_back.value(), k_texture_bytes, 17));

    ctx->advance_epoch_and_wait_for_idle();
}
