#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh> // cc::byte, cc::u32
#include <nexus/test.hh>
#include <shaped-graphics/binding.hh>
#include <shaped-graphics/binding_group.hh> // sg::named_view
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/exceptions.hh>
#include <shaped-graphics/raw_buffer.hh>
#include <shaped-graphics/raw_texture.hh>
#include <shaped-graphics/types.hh>

// Goal of this topic (see docs/error-handling.md): pin that every backend's PUBLIC entry point performs
// its cheap contract validation, so it can't be silently dropped. Each test is an INVOCABLE_TEST run
// against every available backend (see tests/backends/backends.cc).
//
//   - CHECK_ASSERTS(expr): the expr must trip a CC_ASSERT (contract violation — null args, bad bounds,
//     missing usage flags, ...). NOTE: when assertions are compiled out (release), CHECK_ASSERTS reports
//     success WITHOUT running expr, so this topic effectively validates in debug / relwithdebinfo.
//   - CHECK_THROWS_AS(expr, T): the throwing façade must raise the typed sg exception T.
//
// Deliberately NOT covered here (documented so the gap is visible, not accidental):
//   - Out-of-memory sg::allocation_exception and descriptor-heap exhaustion: provoking them is expensive
//     / non-deterministic. The recoverable persistent-descriptor-exhaustion path is exercised on the GPU
//     by backends/dx12/tests/dx12-compute-test.cc ("persistent binding groups free and reuse ...").
//   - compute dispatch / bind validation: needs a full pipeline + shader; covered by dx12-compute-test.
//   - wrong-epoch submit/drop and negative advance_epoch(allowed_in_flight): the latter asserts only
//     after mutating epoch state, so it isn't cleanly catchable by CHECK_ASSERTS; both are lifecycle
//     contracts left to the per-backend suites.
//   - On the vulkan stub, the recording ops are not implemented yet (they CC_UNREACHABLE), so on a vulkan
//     run the command-list CHECK_ASSERTS below trip that not-implemented abort rather than the validation
//     — real enforcement is on dx12 until vulkan implements the ops.

namespace
{
sg::buffer_usage const copy_dst = sg::buffer_usage::copy_dst;
sg::buffer_usage const copy_src = sg::buffer_usage::copy_src;
sg::buffer_usage const copy_both = sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst;
} // namespace

INVOCABLE_TEST("sg error handling - buffer creation validates its size", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    // Size must be >= 0. The check lives in the backend; the public entry must reach it.
    CHECK_ASSERTS(ctx->persistent.try_create_raw_buffer(-1, sg::buffer_usage::none));
    CHECK_ASSERTS(ctx->transient.try_create_raw_buffer(-16, sg::buffer_usage::none));
}

INVOCABLE_TEST("sg error handling - buffer view factories validate usage and bounds", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    // A copy-only buffer: it has none of the shader-view usages, so every typed view must be rejected.
    auto buf = ctx->persistent.create_raw_buffer(256, copy_both);
    REQUIRE(buf != nullptr);

    CHECK_ASSERTS(buf->as_uniform_buffer<cc::u32[4]>()); // lacks uniform_buffer usage
    CHECK_ASSERTS(buf->as_readonly_buffer<cc::u32>());   // lacks readonly_buffer usage
    CHECK_ASSERTS(buf->as_readwrite_buffer<cc::u32>());  // lacks readwrite_buffer usage

    // A readonly buffer accepts a readonly view but must still reject an out-of-range or negative range.
    auto ro = ctx->persistent.create_raw_buffer(256, sg::buffer_usage::readonly_buffer);
    REQUIRE(ro != nullptr);
    CHECK_ASSERTS(ro->as_readonly_buffer<cc::u32>({.offset = 0, .size = 1024})); // 1024 * 4 > 256 bytes
    CHECK_ASSERTS(ro->as_readonly_buffer<cc::u32>({.offset = -1, .size = 1}));   // negative offset
}

INVOCABLE_TEST("sg error handling - uniform view requires 256-byte-aligned offset", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto ub = ctx->persistent.create_raw_buffer(1024, sg::buffer_usage::uniform_buffer);
    REQUIRE(ub != nullptr);

    // A uniform block offset must be 256-byte aligned.
    CHECK_ASSERTS(ub->as_uniform_buffer<cc::u32[4]>(128));
}

INVOCABLE_TEST("sg error handling - texture creation validates its shape", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    using td = sg::texture_description;
    auto const usage = sg::texture_usage::copy_dst;

    CHECK_ASSERTS(ctx->persistent.try_create_raw_texture(td{.format = sg::pixel_format::undefined, .usage = usage}));
    CHECK_ASSERTS(ctx->persistent.try_create_raw_texture(
        td{.format = sg::pixel_format::rgba8_unorm, .width = 0, .usage = usage})); // extent must be >= 1
    CHECK_ASSERTS(ctx->persistent.try_create_raw_texture(
        td{.format = sg::pixel_format::rgba8_unorm, .mip_levels = 0, .usage = usage})); // mip >= 1
    CHECK_ASSERTS(ctx->persistent.try_create_raw_texture(
        td{.format = sg::pixel_format::rgba8_unorm, .sample_count = 0, .usage = usage})); // sample_count >= 1
    // Multisampling is 2D only: a 3D MSAA texture is a shape contradiction.
    CHECK_ASSERTS(ctx->persistent.try_create_raw_texture(td{.format = sg::pixel_format::rgba8_unorm,
                                                            .dimension = sg::texture_dimension::d3,
                                                            .sample_count = 4,
                                                            .usage = usage}));
}

INVOCABLE_TEST("sg error handling - inline upload validates its arguments", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto dst = ctx->persistent.create_raw_buffer(256, copy_dst);    // valid upload target
    auto no_dst = ctx->persistent.create_raw_buffer(256, copy_src); // lacks copy_dst
    REQUIRE(dst != nullptr);
    REQUIRE(no_dst != nullptr);

    cc::byte payload[16] = {};
    auto const bytes = cc::span<cc::byte const>(payload, 16);

    auto cmd = ctx->create_command_list();

    CHECK_ASSERTS(cmd->upload.bytes_to_buffer(nullptr, bytes));  // null target
    CHECK_ASSERTS(cmd->upload.bytes_to_buffer(no_dst, bytes));   // missing copy_dst usage
    CHECK_ASSERTS(cmd->upload.bytes_to_buffer(dst, bytes, 250)); // 250 + 16 > 256 (out of bounds)
    CHECK_ASSERTS(cmd->upload.bytes_to_buffer(dst, bytes, -1));  // negative offset

    ctx->drop_command_list(cc::move(cmd));
}

INVOCABLE_TEST("sg error handling - inline download validates its arguments", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto src = ctx->persistent.create_raw_buffer(256, copy_src);    // valid download source
    auto no_src = ctx->persistent.create_raw_buffer(256, copy_dst); // lacks copy_src
    REQUIRE(src != nullptr);
    REQUIRE(no_src != nullptr);

    auto cmd = ctx->create_command_list();

    CHECK_ASSERTS(cmd->download.bytes_from_buffer(nullptr, 0, 16)); // null source
    CHECK_ASSERTS(cmd->download.bytes_from_buffer(no_src, 0, 16));  // missing copy_src usage
    CHECK_ASSERTS(cmd->download.bytes_from_buffer(src, 0, -1));     // negative size
    CHECK_ASSERTS(cmd->download.bytes_from_buffer(src, 250, 16));   // 250 + 16 > 256 (out of bounds)

    ctx->drop_command_list(cc::move(cmd));
}

INVOCABLE_TEST("sg error handling - buffer copy validates its arguments", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto src = ctx->persistent.create_raw_buffer(256, copy_src);
    auto dst = ctx->persistent.create_raw_buffer(256, copy_dst);
    auto both = ctx->persistent.create_raw_buffer(256, copy_both);
    REQUIRE(src != nullptr);
    REQUIRE(dst != nullptr);
    REQUIRE(both != nullptr);

    auto cmd = ctx->create_command_list();

    CHECK_ASSERTS(cmd->copy.buffer_bytes_region({.src = nullptr, .dst = dst, .size_in_bytes = 16})); // null src
    CHECK_ASSERTS(cmd->copy.buffer_bytes_region({.src = src, .dst = nullptr, .size_in_bytes = 16})); // null dst
    CHECK_ASSERTS(cmd->copy.buffer_bytes_region({.src = dst, .dst = dst, .size_in_bytes = 16})); // src lacks copy_src
    CHECK_ASSERTS(cmd->copy.buffer_bytes_region({.src = src, .dst = src, .size_in_bytes = 16})); // dst lacks copy_dst
    CHECK_ASSERTS(cmd->copy.buffer_bytes_region({.src = src, .dst = dst, .size_in_bytes = -1})); // negative size
    CHECK_ASSERTS(cmd->copy.buffer_bytes_region(
        {.src = src, .dst = dst, .size_in_bytes = 16, .src_offset_in_bytes = 250})); // src out of bounds
    // Same-buffer copy with overlapping [0,128) and [64,192) ranges.
    CHECK_ASSERTS(cmd->copy.buffer_bytes_region(
        {.src = both, .dst = both, .size_in_bytes = 128, .src_offset_in_bytes = 0, .dst_offset_in_bytes = 64}));

    ctx->drop_command_list(cc::move(cmd));
}

INVOCABLE_TEST("sg error handling - transient budget must be positive", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    CHECK_ASSERTS(ctx->transient.set_budget(0));
    CHECK_ASSERTS(ctx->transient.set_budget(-1));
}

INVOCABLE_TEST("sg error handling - submit and drop reject a null command list", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    CHECK_ASSERTS(ctx->submit_command_list(nullptr));
    CHECK_ASSERTS(ctx->drop_command_list(nullptr));
}

INVOCABLE_TEST("sg error handling - advance rejects an epoch with open command lists", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto cmd = ctx->create_command_list();
    // A command list opened this epoch must be submitted or dropped before advancing. The assert is at
    // the top of advance_epoch (before any state change), so it is cleanly catchable here.
    CHECK_ASSERTS(ctx->advance_epoch_and_wait_for_idle());
    ctx->drop_command_list(cc::move(cmd)); // clean up so the context stays usable
}

INVOCABLE_TEST("sg error handling - binding group wiring errors throw", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    // A one-entry layout expecting a read-write structured buffer named "Data".
    sg::binding const b{
        .name = "Data",
        .set = 0,
        .index = 0,
        .count = 1,
        .type = sg::binding_type::readwrite_structured_buffer,
    };
    auto layout = ctx->uncached.create_binding_group_layout(cc::span<sg::binding const>(&b, 1));
    REQUIRE(layout != nullptr);

    // The buffer carries both accesses so we can build a matching AND a mismatched view.
    auto buf
        = ctx->persistent.create_raw_buffer(256, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::readonly_buffer);
    REQUIRE(buf != nullptr);

    // A view bound to a name the layout does not declare.
    sg::named_view const unknown_name{.name = "Nope", .view = buf->as_readwrite_buffer<cc::u32>()};
    CHECK_THROWS_AS(ctx->persistent.create_binding_group(layout, cc::span<sg::named_view const>(&unknown_name, 1)),
                    sg::binding_group_exception);
    // The fallible core surfaces the same failure as an error rather than throwing.
    CHECK(ctx->persistent.try_create_binding_group(layout, cc::span<sg::named_view const>(&unknown_name, 1)).has_error());

    // A read-only view bound to a read-write binding: right name, wrong kind.
    sg::named_view const wrong_kind{.name = "Data", .view = buf->as_readonly_buffer<cc::u32>()};
    CHECK_THROWS_AS(ctx->persistent.create_binding_group(layout, cc::span<sg::named_view const>(&wrong_kind, 1)),
                    sg::binding_group_exception);

    // The layout's "Data" binding is never provided.
    CHECK_THROWS_AS(ctx->persistent.create_binding_group(layout, cc::span<sg::named_view const>()),
                    sg::binding_group_exception);
    CHECK(ctx->transient.try_create_binding_group(layout, cc::span<sg::named_view const>()).has_error());
}
