#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

#include <cstdint>
#include <memory>
#include <type_traits>

// The shared `*_handle` typedefs are std::shared_ptr to the sg types. command_list has no handle:
// it is a move-only temporary held by std::unique_ptr<sg::command_list> and passed by reference.
static_assert(std::is_same_v<sg::context_handle, std::shared_ptr<sg::context>>);
static_assert(std::is_same_v<sg::buffer_handle, std::shared_ptr<sg::buffer>>);

// Epoch / submission-token sentinels: 64-bit, invalid == 0, and deliberately-high first values so a
// zero-initialized token is obviously wrong; not_submitted is all-ones (always "not yet complete").
static_assert(sizeof(sg::epoch) == 8 && sizeof(sg::submission_token) == 8);
static_assert(std::uint64_t(sg::epoch::invalid) == 0 && std::uint64_t(sg::epoch::first) == 10000);
static_assert(std::uint64_t(sg::submission_token::invalid) == 0);
static_assert(std::uint64_t(sg::submission_token::first) == 30000);
static_assert(std::uint64_t(sg::submission_token::not_submitted) == ~std::uint64_t(0));

// context/command_list/buffer are abstract interfaces backends derive from; context is
// non-instantiable (pure-virtual factories), and the recording/creation entry points are stubbed
// with CC_UNREACHABLE (they abort by design). So these tests only touch the implemented parts —
// here, the shape metadata the buffer base carries — via a minimal concrete subclass.

namespace
{
struct test_buffer final : sg::buffer
{
    test_buffer(cc::isize size_in_bytes, sg::buffer_usage usage) : sg::buffer(size_in_bytes, usage) {}
};
} // namespace

TEST("sg smoke - buffer shape")
{
    // The shape metadata lives in the buffer base (protected, inline accessors); a real backend
    // buffer inherits exactly this.
    auto const b = test_buffer(256, sg::buffer_usage::vertex_buffer | sg::buffer_usage::copy_dst);
    CHECK(b.size_in_bytes() == 256);
    CHECK(sg::has_flag(b.usage(), sg::buffer_usage::vertex_buffer));
    CHECK(sg::has_flag(b.usage(), sg::buffer_usage::copy_dst));
    CHECK(!sg::has_flag(b.usage(), sg::buffer_usage::index_buffer));
}

TEST("sg smoke - empty buffer shape")
{
    // Size 0 is valid — an empty buffer, like an empty span. The base only rejects negative sizes.
    auto const b = test_buffer(0, sg::buffer_usage::none);
    CHECK(b.size_in_bytes() == 0);
}

TEST("sg smoke - backend kind")
{
    CHECK(int(sg::backend_kind::dx12) != int(sg::backend_kind::vulkan));
}
