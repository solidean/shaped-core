#include "dx12-test-common.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_native_scope.hh>

using namespace cc::primitive_defines;

// dx12_native_scope: foreign code recording onto an sg command list.
//
// The foreign call here is a plain CopyResource, which needs no descriptors and no pipeline.
// What is under test is the bracket around it rather than the call, and two things have to hold across it:
//   - the declared accesses are barriered BEFORE the foreign code runs, so its copy reads a source sg last wrote;
//   - the declared accesses become the resources' state, so sg's next operation barriers from there rather than from
//     what it last recorded itself.
// A missing barrier shows up twice: the debug layer says so (and the log rule fails the test on it), and the bytes
// come back wrong.

namespace
{
namespace dx12 = sg::backend::dx12;

constexpr isize k_size = 256;
} // namespace

ASYNC_INVOCABLE_TEST("sg dx12 - a native scope brackets foreign work in one list",
                     (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto src = c.persistent.create_raw_buffer(k_size, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    auto dst = c.persistent.create_raw_buffer(k_size, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(src != nullptr);
    REQUIRE(dst != nullptr);

    byte pattern[k_size];
    for (auto i = 0; i < k_size; ++i)
        pattern[i] = byte(i);

    // One list for all three, so the barriers under test are the ones this list emits rather than the decay a submit
    // would give for free.
    auto cmd = c.create_command_list();
    REQUIRE(cmd != nullptr);
    cmd->upload.bytes_to_buffer(src, cc::span<byte const>(pattern, k_size));

    {
        auto const native = dx12::dx12_native_scope::open(
            *cmd, {},
            {{.buffer = src, .access = sg::access_flag::copy_read, .stages = sg::pipeline_stage_flag::copy},
             {.buffer = dst, .access = sg::access_flag::copy_write, .stages = sg::pipeline_stage_flag::copy}});

        CHECK(native.list() != nullptr);
        CHECK(native.device() != nullptr);
        native.list()->CopyResource(native.resource(dst), native.resource(src));
    }

    auto const future = cmd->download.bytes_from_buffer(dst, 0, k_size);
    c.submit_command_list(cc::move(cmd));

    auto const bytes = co_await future.bytes();
    REQUIRE(bytes.size() == k_size);

    auto matches = true;
    for (auto i = 0; i < k_size; ++i)
        if (bytes[i] != byte(i))
            matches = false;
    CHECK(matches);
}

TEST("sg dx12 - a native access names the layout its texture must be in")
{
    // Write beats read, and the shader family beats the copy one: foreign code given both does the write, and a
    // layout that only admits reads would be wrong for it.
    CHECK(dx12::native_layout_for(sg::access_flag::shader_read) == sg::texture_layout::shader_readonly);
    CHECK(dx12::native_layout_for(sg::access_flag::shader_write) == sg::texture_layout::shader_readwrite);
    CHECK(dx12::native_layout_for(sg::access_flag::shader_read | sg::access_flag::shader_write)
          == sg::texture_layout::shader_readwrite);
    CHECK(dx12::native_layout_for(sg::access_flag::copy_read) == sg::texture_layout::copy_src);
    CHECK(dx12::native_layout_for(sg::access_flag::copy_write) == sg::texture_layout::copy_dst);
    CHECK(dx12::native_layout_for(sg::access_flag::shader_read | sg::access_flag::copy_write)
          == sg::texture_layout::copy_dst);

    // An access the table does not name asks for the layout every access can use.
    CHECK(dx12::native_layout_for({}) == sg::texture_layout::general);
    CHECK(dx12::native_layout_for(sg::access_flag::uniform_read) == sg::texture_layout::general);
}
