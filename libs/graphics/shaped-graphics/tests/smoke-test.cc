#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

#include <memory>
#include <type_traits>

// The public `*_handle` typedefs are std::shared_ptr to the sg types.
static_assert(std::is_same_v<sg::context_handle, std::shared_ptr<sg::context>>);
static_assert(std::is_same_v<sg::command_list_handle, std::shared_ptr<sg::command_list>>);
static_assert(std::is_same_v<sg::buffer_handle, std::shared_ptr<sg::buffer>>);

// Most of sg is stubbed with CC_UNREACHABLE — those bodies abort by design, so these tests only
// touch the parts that are actually implemented (value types, enums, type identities) and never
// call a stubbed entry point.

namespace
{
// Minimal concrete backend_buffer so the shape test can build a buffer without a real backend.
struct dummy_backend_buffer final : sg::backend_buffer
{
};
} // namespace

TEST("sg smoke - buffer shape")
{
    // buffer keeps its shape metadata inline (real), fronting a backend-owned GPU resource.
    auto const b = sg::buffer(256, sg::buffer_usage::vertex | sg::buffer_usage::copy_dst,
                              std::make_shared<dummy_backend_buffer>());
    CHECK(b.size_in_bytes() == 256);
    CHECK(sg::has_flag(b.usage(), sg::buffer_usage::vertex));
    CHECK(sg::has_flag(b.usage(), sg::buffer_usage::copy_dst));
    CHECK(!sg::has_flag(b.usage(), sg::buffer_usage::index));
}

TEST("sg smoke - backend kind")
{
    CHECK(int(sg::backend_kind::dx12) != int(sg::backend_kind::vulkan));
}
