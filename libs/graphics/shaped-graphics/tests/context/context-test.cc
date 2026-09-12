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

// The async forms of the two completion questions, and the one blocking spelling that is left.
//
// The point of each is that a caller can learn a thing has finished WITHOUT a thread stopping, which is the whole
// reason the blocking family is going away: a browser cannot stop a thread at all.
INVOCABLE_TEST("sg - an epoch's completion is readable as an async", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    // An epoch already retired answers with a node that is ready, so a caller never special-cases the past.
    auto const past = ctx->epoch_completion(ctx->completed_epoch());
    REQUIRE(past != nullptr);
    CHECK(past->is_ready());

    // One that has not closed yet cannot be ready, and asking twice hands back the same node rather than two.
    auto const open = ctx->current_epoch();
    auto const pending = ctx->epoch_completion(open);
    REQUIRE(pending != nullptr);
    CHECK(!pending->is_ready());
    CHECK(ctx->epoch_completion(open) == pending);

    // Closing it and draining settles that node — no wait_for_epoch anywhere in sight.
    ctx->advance_epoch(0);
    ctx->process_completed_epochs();
    CHECK(pending->is_ready());
}

INVOCABLE_TEST("sg - a submission's completion is readable as an async", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    // Never submitted, so its completion never arrives — matching what the poll reports rather than claiming done.
    auto const never = ctx->submission_completion(sg::submission_token::not_submitted);
    REQUIRE(never != nullptr);
    CHECK(!never->is_ready());
    CHECK(!ctx->is_submission_complete(sg::submission_token::not_submitted));

    auto cmd = ctx->create_command_list();
    REQUIRE(cmd != nullptr);
    auto const token = ctx->submit_command_list(cc::move(cmd));

    auto const done = ctx->submission_completion(token);
    REQUIRE(done != nullptr);

    ctx->block_until_idle();
    CHECK(ctx->is_submission_complete(token));
    CHECK(done->is_ready());
}

// The non-blocking throttle: the same pipelining bound advance_epoch expresses by waiting, expressed as a decision.
INVOCABLE_TEST("sg - try_advance_epoch declines instead of waiting", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    // Drained, so nothing is in flight and any budget admits an advance.
    ctx->advance_epoch(0);
    CHECK(ctx->in_flight_epoch_count() == 0);

    auto const before = ctx->current_epoch();
    CHECK(ctx->try_advance_epoch(2));
    CHECK(u64(ctx->current_epoch()) > u64(before));

    // A budget of zero admits an advance only while nothing is in flight, and the advance above left one epoch there
    // unless the GPU has already retired it -- so this is allowed to go either way, and what must hold is that it
    // never blocks and never advances past its own bound.
    auto const at_budget = ctx->current_epoch();
    if (!ctx->try_advance_epoch(0))
        CHECK(ctx->current_epoch() == at_budget);

    ctx->block_until_idle();
}

// block_until_idle is the only blocking spelling left, and it has to mean more than "the GPU is idle": the readback
// actor delivers a download's bytes on its own thread, after the copy the GPU already finished.
INVOCABLE_TEST("sg - block_until_idle drains the actors, not just the GPU", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    REQUIRE(ctx->execution() == sg::execution_model::may_block);

    auto const src = ctx->persistent.create_buffer<u32>(4, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);

    auto cmd = ctx->create_command_list();
    u32 const values[] = {1, 2, 3, 4};
    cmd->upload.data_to_buffer(src, cc::span<u32 const>(values));
    auto const future = cmd->download.data_from_buffer(src);
    (void)ctx->submit_command_list(cc::move(cmd));

    ctx->advance_epoch(0);
    ctx->block_until_idle();

    // Delivered, without any wait_for(future) — which is the guarantee the blocking download API used to be the only
    // source of.
    REQUIRE(future.is_ready());
    auto const data = future.try_get_data();
    REQUIRE(data.has_value());
    REQUIRE(data.value().size() == 4);
    CHECK(data.value()[0] == 1);
    CHECK(data.value()[3] == 4);
}
