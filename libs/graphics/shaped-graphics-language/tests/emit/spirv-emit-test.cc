#include "emit-test-support.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <nexus/source_root.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>

#include <cstdlib>
#include <filesystem>

using namespace cc::primitive_defines;

// SGL's own SPIR-V: what a module holds, and what `spirv-val` says of every compute entry point the fixtures hold.

namespace
{
constexpr cc::string_view k_doubled = "binding work:\n"
                                      "    values: mut buffer[float]\n"
                                      "\n"
                                      "@compute(64) fun run(@thread_id id: int3){work}:\n"
                                      "    work.values[id.x] = work.values[id.x] * 2.0 + 1.0\n";

constexpr cc::string_view k_precise = "binding work:\n"
                                      "    values: mut buffer[float]\n"
                                      "\n"
                                      "@float(.precise) @compute(64) fun run(@thread_id id: int3){work}:\n"
                                      "    work.values[id.x] = work.values[id.x] * 2.0 + 1.0\n";

constexpr cc::string_view k_rendered = "@vertex struct input:\n"
                                       "    at: float3\n"
                                       "\n"
                                       "@vertex fun run(v: input) -> hpos4 => hpos4(v.at.x, v.at.y, v.at.z, 1.0)\n";

[[nodiscard]] cc::result<sgl::emitted_source, cc::string> written(cc::string_view source,
                                                                  cc::string_view entry,
                                                                  sgl::emit::native_route route)
{
    return sgl::compile_to_text(
        {.source = source, .entry_point = entry, .target = sgl::emit::target::hlsl_vulkan, .native = route});
}

/// How many times the module decorates something with `decoration`, which takes no operands.
[[nodiscard]] isize decorated(cc::span<u32 const> words, u32 decoration)
{
    auto count = isize(0);
    for (auto at = isize(5); at < words.size(); at += isize(words[at] >> 16))
        if ((words[at] & 0xffffu) == 71 && words[at + 2] == decoration)
            ++count;
    return count;
}

/// The name of every `.sgl` file under `root`, sorted, so a failure reads the same on every run.
[[nodiscard]] cc::vector<cc::string> files_under(cc::string_view root)
{
    namespace fs = std::filesystem;
    auto found = cc::vector<cc::string>();
    auto ec = std::error_code();
    for (auto const& e : fs::directory_iterator(fs::path(root.data(), root.data() + root.size()), ec))
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

void write_words(cc::string_view path, cc::span<u32 const> words)
{
    auto adapter = cc::file_write_stream_adapter::create(path);
    REQUIRE(adapter.has_value());
    auto stream = adapter.value().stream();
    REQUIRE(stream.write(words.as_bytes()).has_value());
    REQUIRE(stream.flush().has_value());
}
} // namespace

TEST("sgl spirv - a module is SPIR-V 1.6, and states its workgroup")
{
    auto const e = written(k_doubled, "run", sgl::emit::native_route::require);
    REQUIRE(e.has_value());
    auto const& words = e.value().spirv;
    REQUIRE(words.size() > 5);
    CHECK(words[0] == 0x07230203u);
    CHECK(words[1] == 0x00010600u);
    // a reader of the program sees what the words were written from
    CHECK(e.value().text.contains("entry run [64 1 1]"));
    CHECK(e.value().text.contains("fmul"));
}

TEST("sgl spirv - an entry point's guarantee stands on each float operation it keeps (EMIT-185)")
{
    auto const fast = written(k_doubled, "run", sgl::emit::native_route::require);
    auto const precise = written(k_precise, "run", sgl::emit::native_route::require);
    REQUIRE(fast.has_value());
    REQUIRE(precise.has_value());
    CHECK(decorated(fast.value().spirv, 42) == 0);
    CHECK(decorated(precise.value().spirv, 42) == 2);
}

TEST("sgl spirv - what has no lowering yet is named, and a preference falls back to the text")
{
    auto const required = written(k_rendered, "run", sgl::emit::native_route::require);
    REQUIRE(required.has_error());
    CHECK(required.error().contains("which the SPIR-V route does not lower yet"));

    auto const preferred = written(k_rendered, "run", sgl::emit::native_route::prefer);
    REQUIRE(preferred.has_value());
    CHECK(preferred.value().spirv.empty());
    CHECK(preferred.value().native_gap.contains("vertex"));
    CHECK(!preferred.value().text.empty());
}

TEST("sgl spirv - every compute entry point of the fixtures that lowers is what spirv-val calls valid")
{
#if defined(CC_OS_EMSCRIPTEN)
    SKIP("a browser has no spirv-val to run");
#else
    // dev.py puts the pinned Vulkan SDK's tools on the path
#if defined(CC_OS_WINDOWS)
    constexpr auto probe = "spirv-val --version > NUL 2>&1";
#else
    constexpr auto probe = "spirv-val --version > /dev/null 2>&1";
#endif
    if (std::system(probe) != 0)
        SKIP("no spirv-val on the path");

    auto const module_path = cc::format("{}/sgl-spirv-emit-test.spv", cc::temp_directory_path());
    auto const command = cc::format("spirv-val --target-env vulkan1.3 --scalar-block-layout \"{}\"", module_path);
    auto command_text = cc::vector<char>::create_copy_of(cc::span<char const>(command.data(), command.size()));
    command_text.push_back('\0');
    auto files = files_under(nx::source_path(SGL_SG_SHADERS_DIR));
    files.push_back_range(files_under(nx::source_path(SGL_SR_SHADERS_DIR)));
    REQUIRE(!files.empty());

    auto lowered = 0;
    for (auto const& file : files)
    {
        auto const source = read_all(file);
        // what names a module of another directory is the module tests', and this one reads a file alone
        auto const all = sgl::compile_all_to_text({.source = source, .targets = {sgl::emit::target::hlsl_vulkan}});
        if (all.has_error())
            continue;
        for (auto const& entry : all.value())
        {
            auto const e = sgl::compile_to_text({.source = source,
                                                 .entry_point = entry.entry_point,
                                                 .stage = sgl::check::stage::compute,
                                                 .target = sgl::emit::target::hlsl_vulkan,
                                                 .native = sgl::emit::native_route::prefer});
            if (e.has_error() || e.value().spirv.empty())
                continue;
            ++lowered;
            write_words(module_path, e.value().spirv);
            CHECK(std::system(command_text.data()) == 0).dump("file", file).dump("entry", entry.entry_point);
        }
    }
    // the number the slice reaches, so a lowering lost shows here rather than as a quiet fall back to DXC
    CHECK(lowered >= 60);
#endif
}
