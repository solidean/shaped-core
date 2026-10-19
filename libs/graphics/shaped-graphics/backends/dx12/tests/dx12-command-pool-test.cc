#include "dx12-test-common.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>

// Backend-internal invariant tests for the per-queue command allocator / command list pool.
// These reach into concrete dx12 types (c._cmd_pool) to assert recycling behaviour the public sg API does not expose — the kind of test that only belongs in a backend suite.
// See libs/graphics/shaped-graphics/docs/concepts/backends.md.
// Allocators are epoch-gated, recycled once the epoch retires; lists are not, and are returned at submit.
// They run on the shared context, so they assert changes in the pool's counts, after a first cycle that leaves one of each free.

namespace
{
namespace dx12 = sg::backend::dx12;
} // namespace

ASYNC_INVOCABLE_TEST("sg dx12 - command allocators are recycled across epochs", (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto const free_count = [&] { return c._cmd_pool.free_allocator_count(D3D12_COMMAND_LIST_TYPE_DIRECT); };

    auto first = c.create_dx12_command_list();
    REQUIRE(first.has_value());
    c.submit_dx12_command_list(cc::move(first.value()));
    c.advance_epoch();
    co_await c.idle_completion();
    auto const pooled = free_count();
    REQUIRE(pooled >= 1); // the retired epoch returned the first list's allocator

    // The next list reuses a pooled allocator rather than creating a new one.
    auto cmd = c.create_dx12_command_list();
    REQUIRE(cmd.has_value());
    CHECK(free_count() == pooled - 1);
    c.submit_dx12_command_list(cc::move(cmd.value()));
    CHECK(free_count() == pooled - 1); // still in flight — captured by the current epoch

    c.advance_epoch();
    co_await c.idle_completion();
    CHECK(free_count() == pooled); // reset and returned to the free pool on retire
}

INVOCABLE_TEST("sg dx12 - command lists are pooled and reused", (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto const free_lists = [&] { return c._cmd_pool.free_command_list_count(D3D12_COMMAND_LIST_TYPE_DIRECT); };

    // A submitted list is closed and returned to the pool immediately (lists are not epoch-gated).
    // One of them: this list touches no resource, so it never asks for a pre-list.
    auto first = c.create_dx12_command_list();
    REQUIRE(first.has_value());
    c.submit_dx12_command_list(cc::move(first.value()));
    auto const pooled = free_lists();
    REQUIRE(pooled >= 1);

    // The next create reuses the pooled list (cheap reset) rather than creating a fresh one.
    auto cmd = c.create_dx12_command_list();
    REQUIRE(cmd.has_value());
    CHECK(free_lists() == pooled - 1);
    c.submit_dx12_command_list(cc::move(cmd.value()));
    CHECK(free_lists() == pooled);
}

INVOCABLE_TEST("sg dx12 - a dropped list returns its allocator and list to the pool immediately",
               (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto const free_allocs = [&] { return c._cmd_pool.free_allocator_count(D3D12_COMMAND_LIST_TYPE_DIRECT); };
    auto const free_lists = [&] { return c._cmd_pool.free_command_list_count(D3D12_COMMAND_LIST_TYPE_DIRECT); };

    auto first = c.create_dx12_command_list();
    REQUIRE(first.has_value());
    c.drop_dx12_command_list(cc::move(first.value()));
    auto const pooled_allocs = free_allocs();
    auto const pooled_lists = free_lists();
    REQUIRE(pooled_allocs >= 1);
    REQUIRE(pooled_lists >= 1);

    auto cmd = c.create_dx12_command_list();
    REQUIRE(cmd.has_value());
    CHECK(free_allocs() == pooled_allocs - 1);
    CHECK(free_lists() == pooled_lists - 1);
    c.drop_dx12_command_list(cc::move(cmd.value()));

    // Never submitted, so the GPU never touched the allocator: both go straight back, no epoch needed.
    CHECK(free_allocs() == pooled_allocs);
    CHECK(free_lists() == pooled_lists);
}
