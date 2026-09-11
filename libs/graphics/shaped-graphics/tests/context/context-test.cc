#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/fwd.hh>            // cc::u64: epoch is an enum over u64
#include <nexus/test.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/types.hh>

using namespace cc::primitive_defines;

// Backend-agnostic sg API tests.
// Each is an INVOCABLE_TEST taking a live context, so it runs against every available backend.
// The per-backend entry driver (tests/backends/<backend>-entry.cc) creates a context and invokes all of these.
// An alias (tests/backends/backends.cc) makes each runnable by its own name across backends, e.g. `uv run dev.py test "sg - context is live"`.
// To target one backend, run its driver: `dev.py test "sg dx12 warp backend" -c dx12-warp "sg - context is live"`.

INVOCABLE_TEST("sg - context is live", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    CHECK(!ctx->is_shut_down());
}

INVOCABLE_TEST("sg - accepts at least one shader format", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto const formats = ctx->accepted_shader_formats();
    REQUIRE(!formats.empty());

    // accepts_shader_format is the list, queried one at a time.
    for (auto format : formats)
        CHECK(ctx->accepts_shader_format(format));

    // Each backend takes its own bytecode and nothing else.
    // backend_kind is non-exhaustive, so a backend we do not name here only has to satisfy the checks above.
    switch (ctx->backend())
    {
    case sg::backend_kind::dx12:
        CHECK(ctx->accepts_shader_format(sg::shader_format::dxil));
        CHECK(!ctx->accepts_shader_format(sg::shader_format::spirv));
        break;
    case sg::backend_kind::vulkan:
        CHECK(ctx->accepts_shader_format(sg::shader_format::spirv));
        CHECK(!ctx->accepts_shader_format(sg::shader_format::dxil));
        break;
    default:
        break;
    }
}

INVOCABLE_TEST("sg - advances an epoch", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto const before = ctx->current_epoch();
    ctx->advance_epoch_and_wait_for_idle();
    CHECK(u64(ctx->current_epoch()) > u64(before));
    CHECK(u64(ctx->completed_epoch()) >= u64(before)); // the epoch we started in is now done
}

INVOCABLE_TEST("sg - completed epoch trails current across advances", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    // Draining (advance_epoch(0)) leaves nothing in flight: the just-closed epoch is completed, and
    // completed never overtakes current.
    for (int i = 0; i < 3; ++i)
    {
        auto const closing = ctx->current_epoch();
        ctx->advance_epoch(0); // fully drain the GPU
        CHECK(u64(ctx->current_epoch()) > u64(closing));
        CHECK(u64(ctx->completed_epoch()) >= u64(closing));
        CHECK(u64(ctx->completed_epoch()) < u64(ctx->current_epoch()));
    }
}

INVOCABLE_TEST("sg - epoch waits and reclaim are safe to call", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    // With nothing in flight these are no-ops, but must not fault or move the epoch backwards.
    ctx->process_completed_epochs();
    ctx->wait_for_next_inflight_epoch();
    ctx->wait_for_epoch(ctx->completed_epoch());
    CHECK(u64(ctx->completed_epoch()) <= u64(ctx->current_epoch()));
}

// ctx.supports() is the single source for a capability, and the per-scope bools forward to it.
// Two spellings that can disagree is the failure this pins: a backend answering "yes" at the recording site and "no"
// at the context is how a caller ends up gating on the wrong one.
INVOCABLE_TEST("sg - capability queries agree with the context", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto cmd = ctx->create_command_list();
    REQUIRE(cmd != nullptr);

    CHECK(cmd->raytracing.is_supported() == ctx->supports(sg::feature::raytracing));
    CHECK(cmd->query.is_supported() == ctx->supports(sg::feature::timestamp_query));
    CHECK(ctx->supports_headless_present() == ctx->supports(sg::feature::headless_present));

    ctx->drop_command_list(cc::move(cmd));
}

// The limits are floors a portable caller sizes against, so they must be reportable and sane on every backend.
INVOCABLE_TEST("sg - limits report the portable floors", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto const& limits = ctx->limits();
    CHECK(limits.max_binding_groups == sg::max_binding_groups);
    CHECK(limits.max_sample_count >= 1);
}
