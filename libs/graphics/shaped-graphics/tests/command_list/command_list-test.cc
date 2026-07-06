#include <clean-core/fwd.hh> // cc::u64
#include <nexus/test.hh>
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/fwd.hh> // sg::submission_token
#include <shaped-graphics/types.hh>

// Backend-agnostic command-list lifecycle: create → submit / drop, epoch stamping, and submission-token
// completion. Run against every available backend (see tests/context/context-test.cc for the mechanism).

INVOCABLE_TEST("sg - a fresh command list is stamped with the current epoch", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto cmd = ctx->create_command_list();
    REQUIRE(cmd.has_value());
    CHECK(cmd.value()->created_in_epoch() == ctx->current_epoch());

    ctx->drop_command_list(cc::move(cmd.value())); // dropping an unsubmitted list is safe
}

INVOCABLE_TEST("sg - a submitted list completes after the GPU drains", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto cmd = ctx->create_command_list();
    REQUIRE(cmd.has_value());
    auto const token = ctx->submit_command_list(cc::move(cmd.value()));

    // not_submitted is the always-pending sentinel and is never complete.
    CHECK(!ctx->is_submission_complete(sg::submission_token::not_submitted));

    ctx->advance_epoch_and_wait_for_idle(); // fully drain: the token's work is now finished
    CHECK(ctx->is_submission_complete(token));
}

INVOCABLE_TEST("sg - submission tokens advance across submits", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto a = ctx->create_command_list();
    REQUIRE(a.has_value());
    auto const first = ctx->submit_command_list(cc::move(a.value()));

    auto b = ctx->create_command_list();
    REQUIRE(b.has_value());
    auto const second = ctx->submit_command_list(cc::move(b.value()));

    // Distinct submissions get distinct, monotonically increasing tokens.
    CHECK(cc::u64(second) > cc::u64(first));

    ctx->advance_epoch_and_wait_for_idle();
    CHECK(ctx->is_submission_complete(first));
    CHECK(ctx->is_submission_complete(second));
}
