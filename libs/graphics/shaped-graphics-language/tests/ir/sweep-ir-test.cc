#include "ir-test-support.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <nexus/source_root.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>

#include <filesystem>

using namespace cc::primitive_defines;

// Every entry point the repo's SGL holds, down both routes, where its stage is lowered yet.
// Where assertions are on a compile verifies its program after lowering and after each pass, so this is what walks the
// verifier over everything; `spirv-val` and DXC's validator only see the shaders sg and sr ship.
// A debug build pays seconds per file for that, so a default run takes every eighth and --thorough takes them all.

namespace
{
[[nodiscard]] cc::vector<cc::string> sources_under(cc::string_view root)
{
    namespace fs = std::filesystem;
    auto found = cc::vector<cc::string>();
    for (auto const& e : fs::recursive_directory_iterator(fs::path(root.data(), root.data() + root.size())))
        if (e.is_regular_file() && e.path().extension() == ".sgl")
            found.push_back(cc::string(e.path().generic_string().c_str()));
    cc::sort(found);
    return found;
}

[[nodiscard]] cc::string read_all(cc::string_view path)
{
    auto adapter = cc::file_read_stream_adapter::open(path);
    REQUIRE(adapter.has_value());
    auto stream = adapter.value().stream();
    auto bytes = stream.read_all();
    REQUIRE(bytes.has_value());
    return cc::string(cc::string_view(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size()));
}
} // namespace

TEST("sgl ir - every entry point of the repo's SGL goes down both native routes",
     nx::config::rerun_on("libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/**"))
{
    auto files = sources_under(nx::source_path(SGL_SG_SHADERS_DIR));
    files.push_back_range(sources_under(nx::source_path(SGL_SR_SHADERS_DIR)));
    files.push_back_range(sources_under(nx::source_path(SGL_CORPUS_DIR)));
    REQUIRE(!files.empty());

    auto const stride = nx::is_thorough() ? 1 : 8;
    auto spirv = 0;
    auto dxil = 0;
    for (auto i = isize(0); i < files.size(); i += stride)
    {
        auto const& file = files[i];
        auto const source = read_all(file);
        // what names a module of another directory is the module tests', and this one reads a file alone
        auto const all = sgl::compile_all_to_text({.source = source, .targets = {sgl::emit::target::hlsl_vulkan}});
        if (all.has_error())
            continue;
        for (auto const& entry : all.value())
            for (auto const target : {sgl::emit::target::hlsl_vulkan, sgl::emit::target::hlsl_dx12})
            {
                auto const e = sgl::compile_to_text({.source = source,
                                                     .entry_point = entry.entry_point,
                                                     .target = target,
                                                     .native = sgl::emit::native_route::prefer});
                if (e.has_error())
                    continue;
                spirv += e.value().spirv.empty() ? 0 : 1;
                dxil += e.value().dxil.empty() ? 0 : 1;
            }
    }
    // the numbers the sweep reaches, so a lowering lost shows here rather than as a quiet fall back to DXC
    CC_LOG_INFO("{} entry points lowered to SPIR-V and {} to DXIL", spirv, dxil);
    CHECK(spirv >= 260 / stride);
    CHECK(dxil >= 230 / stride);
}
