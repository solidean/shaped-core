#include <nexus/test.hh>
#include <shaped-graphics/backend/resource_access.hh>
#include <shaped-graphics/backend/resource_access_state.hh>

// Pure, backend-free unit tests for the three-timeline access-state machine. These pin the freebie /
// hazard logic (what makes barriers minimal) without needing a GPU. Buffers only, so layout stays
// `general` throughout.

using sg::access_flags;
using sg::pipeline_stage_flags;

namespace
{
constexpr auto compute = pipeline_stage_flags::compute;
constexpr auto copy = pipeline_stage_flags::copy;
constexpr auto fragment = pipeline_stage_flags::fragment;

constexpr auto shader_write = access_flags::shader_write;
constexpr auto shader_read = access_flags::shader_read;
constexpr auto copy_read = access_flags::copy_read;
constexpr auto copy_write = access_flags::copy_write;
} // namespace

TEST("sg access_state - is_unordered_write predicate")
{
    CHECK(sg::is_unordered_write(shader_write));
    CHECK(sg::is_unordered_write(copy_write));
    CHECK(!sg::is_unordered_write(shader_read));
    CHECK(!sg::is_unordered_write(copy_read));
    CHECK(!sg::is_unordered_write(access_flags::color_write)); // ROP-ordered, not an unordered write
}

TEST("sg access_state - first write is a freebie")
{
    sg::resource_access_state s;
    s.declare(compute, shader_write);
    CHECK(s.has_pending_declares());

    auto const b = s.flush();
    CHECK(!b.needed); // nothing in flight -> no barrier
    CHECK(!s.has_pending_declares());
    CHECK(s.has_inflight_writes());
}

TEST("sg access_state - read after write emits a RAW barrier")
{
    sg::resource_access_state s;
    s.declare(compute, shader_write);
    (void)s.flush();

    s.declare(copy, copy_read);
    auto const b = s.flush();
    REQUIRE(b.needed);
    CHECK(sg::has_all(b.src_access, shader_write));
    CHECK(sg::has_all(b.dst_access, copy_read));
    CHECK(sg::has_all(b.src_stages, compute));
    CHECK(sg::has_all(b.dst_stages, copy));
}

TEST("sg access_state - read after read is free, new stage syncs only the delta")
{
    sg::resource_access_state s;
    s.declare(compute, shader_write);
    (void)s.flush();

    s.declare(copy, copy_read);
    REQUIRE(s.flush().needed); // RAW

    // Same read again: already barriered -> no barrier.
    s.declare(copy, copy_read);
    CHECK(!s.flush().needed);

    // A new reader stage: only the delta needs syncing (still against the last write).
    s.declare(fragment, shader_read);
    auto const b = s.flush();
    REQUIRE(b.needed);
    CHECK(sg::has_all(b.dst_stages, fragment));
    CHECK(!sg::has_all(b.dst_stages, copy)); // copy read was already barriered
    CHECK(sg::has_all(b.src_access, shader_write));
}

TEST("sg access_state - write after write serializes")
{
    sg::resource_access_state s;
    s.declare(compute, shader_write);
    (void)s.flush();

    s.declare(compute, shader_write);
    auto const b = s.flush();
    REQUIRE(b.needed);
    CHECK(sg::has_all(b.src_access, shader_write));
    CHECK(sg::has_all(b.dst_access, shader_write));
}

TEST("sg access_state - write after read serializes (WAR)")
{
    sg::resource_access_state s;
    s.declare(fragment, shader_read);
    CHECK(!s.flush().needed); // read with no writer in flight is free

    s.declare(compute, shader_write);
    auto const b = s.flush();
    REQUIRE(b.needed);
    CHECK(sg::has_all(b.src_access, shader_read));
    CHECK(sg::has_all(b.dst_access, shader_write));
}

TEST("sg access_state - reset_keep_layout clears timelines")
{
    sg::resource_access_state s;
    s.declare(compute, shader_write);
    (void)s.flush();
    CHECK(s.has_inflight_writes());

    s.reset_keep_layout();
    CHECK(!s.has_inflight_writes());
    CHECK(!s.has_any_inflight_access());
    CHECK(s.curr_layout == sg::texture_layout::general);
}
