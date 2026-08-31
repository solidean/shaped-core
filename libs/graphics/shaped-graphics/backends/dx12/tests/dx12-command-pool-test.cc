#include "dx12-test-common.hh"

#include <nexus/test.hh>

// Backend-internal invariant tests for the per-queue command allocator / command list pool.
// These reach into concrete dx12 types (c._cmd_pool) to assert recycling behaviour the public sg API does not expose — the kind of test that only belongs in a backend suite.
// See libs/graphics/shaped-graphics/docs/concepts/backends.md.
// Allocators are epoch-gated, recycled once the epoch retires; lists are not, and are returned at submit.
// These assert absolute pool counts, so each takes a fresh make_warp_context() rather than the shared one.

namespace
{
namespace dx12 = sg::backend::dx12;
} // namespace

TEST("sg dx12 - command allocators are recycled across epochs")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto const free_count = [&] { return c._cmd_pool.free_allocator_count(D3D12_COMMAND_LIST_TYPE_DIRECT); };

    // Two per list: one for the recorded body, one for the entry barriers prepended at submit.
    auto cmd = c.create_dx12_command_list();
    REQUIRE(cmd.has_value());
    c.submit_dx12_command_list(cc::move(cmd.value()));
    CHECK(free_count() == 0); // still in flight — captured by the current epoch

    c.advance_epoch_and_wait_for_idle();
    CHECK(free_count() == 2); // reset and returned to the free pool on retire

    // The next list reuses the pooled allocators rather than creating new ones.
    auto cmd2 = c.create_dx12_command_list();
    REQUIRE(cmd2.has_value());
    CHECK(free_count() == 0);
    c.submit_dx12_command_list(cc::move(cmd2.value()));
    c.advance_epoch_and_wait_for_idle();
    CHECK(free_count() == 2);
}

TEST("sg dx12 - command lists are pooled and reused")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto const free_lists = [&] { return c._cmd_pool.free_command_list_count(D3D12_COMMAND_LIST_TYPE_DIRECT); };

    CHECK(free_lists() == 0);

    // A submitted list is closed and returned to the pool immediately (lists are not epoch-gated).
    // Two of them: the body and the entry-barrier list prepended to its submit.
    auto cmd = c.create_dx12_command_list();
    REQUIRE(cmd.has_value());
    c.submit_dx12_command_list(cc::move(cmd.value()));
    CHECK(free_lists() == 2);

    // The next create reuses the pooled lists (cheap reset) rather than creating fresh ones.
    auto cmd2 = c.create_dx12_command_list();
    REQUIRE(cmd2.has_value());
    CHECK(free_lists() == 0);
    c.submit_dx12_command_list(cc::move(cmd2.value()));
    CHECK(free_lists() == 2);
}

TEST("sg dx12 - a dropped list returns its allocator and list to the pool immediately")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto const free_allocs = [&] { return c._cmd_pool.free_allocator_count(D3D12_COMMAND_LIST_TYPE_DIRECT); };
    auto const free_lists = [&] { return c._cmd_pool.free_command_list_count(D3D12_COMMAND_LIST_TYPE_DIRECT); };

    auto cmd = c.create_dx12_command_list();
    REQUIRE(cmd.has_value());
    c.drop_dx12_command_list(cc::move(cmd.value()));

    // Never submitted, so the GPU never touched either allocator: all four go straight back, no epoch needed.
    CHECK(free_allocs() == 2);
    CHECK(free_lists() == 2);
}
