#include <clean-core/algorithm/sort.hh>
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <nexus/source_root.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-shader-compiler-dxc/compiler.hh>

#include <filesystem>

using namespace cc::primitive_defines;

// What SGL writes as DXIL has to pass the validator a signed shader passes, though nothing ships through it.

namespace
{
[[nodiscard]] cc::string read_file(cc::string_view path)
{
    auto adapter = cc::file_read_stream_adapter::open(path);
    REQUIRE(adapter.has_value());
    auto stream = adapter.value().stream();
    auto bytes = stream.read_all();
    REQUIRE(bytes.has_value());
    return cc::string(cc::string_view(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size()));
}

struct kernel
{
    cc::string_view path;
    cc::string_view entry;
};

constexpr kernel k_kernels[] = {
    {"libs/graphics/shaped-graphics/tests/shaders/sgl/dispatch.sgl", "stamp"},
    {"libs/graphics/shaped-rendering/shaders/atrous_denoise.sgl", "main_cs"},
    {"libs/graphics/shaped-rendering/shaders/svgf.sgl", "atrous_cs"},
    {"libs/graphics/shaped-rendering/shaders/nn_conv.sgl", "main_cs"},
};

/// A sum of three products, which SGL's reorder of `fast` additions chains, and which `@float` above `fast` keeps.
constexpr cc::string_view k_sums = "binding work:\n"
                                   "    values: mut buffer[float]\n"
                                   "\n"
                                   "@compute(64) fun fast_sum(@thread_id id: int3){work}:\n"
                                   "    let a = work.values[0]\n"
                                   "    work.values[id.x] += a * 2.0 + a * 3.0 + a * 5.0\n"
                                   "\n"
                                   "@float(.precise) @compute(64) fun strict_sum(@thread_id id: int3){work}:\n"
                                   "    let a = work.values[0]\n"
                                   "    work.values[id.x] += a * 2.0 + a * 3.0 + a * 5.0\n";

[[nodiscard]] cc::result<sgl::emitted_source, cc::string> written(cc::string_view source, cc::string_view entry)
{
    return sgl::compile_to_text({.source = source,
                                 .entry_point = entry,
                                 .target = sgl::emit::target::hlsl_dx12,
                                 .native = sgl::emit::native_route::require});
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
} // namespace

TEST("slib - SGL's own DXIL of real kernels passes the validator")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    for (auto const& k : k_kernels)
    {
        auto const source = read_file(nx::source_path(k.path));
        auto const e = written(source, k.entry);
        REQUIRE(e.has_value());
        REQUIRE(!e.value().dxil.empty());
        auto const refused = compiler.value().validate(e.value().dxil);
        CHECK(!refused.has_value()).dump("entry", k.entry).dump("validator", refused.value_or(cc::string()));
    }
}

TEST("slib - every compute entry point of the fixtures that SGL's DXIL lowers passes the validator")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    auto files = files_under(nx::source_path("libs/graphics/shaped-graphics/tests/shaders/sgl"));
    files.push_back_range(files_under(nx::source_path("libs/graphics/shaped-rendering/shaders")));
    REQUIRE(!files.empty());

    auto reached = 0;
    auto lowered = 0;
    for (auto const& file : files)
    {
        auto const source = read_file(file);
        // what names a module of another directory is the module tests', and this one reads a file alone
        auto const all = sgl::compile_all_to_text({.source = source, .targets = {sgl::emit::target::hlsl_dx12}});
        if (all.has_error())
            continue;
        for (auto const& entry : all.value())
        {
            auto const e = sgl::compile_to_text({.source = source,
                                                 .entry_point = entry.entry_point,
                                                 .stage = sgl::check::stage::compute,
                                                 .target = sgl::emit::target::hlsl_dx12,
                                                 .native = sgl::emit::native_route::prefer});
            if (e.has_error())
                continue;
            ++reached;
            if (e.value().dxil.empty())
            {
                CC_LOG_INFO("{}: {}: {}", file, entry.entry_point, e.value().native_gap);
                continue;
            }
            ++lowered;
            auto const refused = compiler.value().validate(e.value().dxil);
            CHECK(!refused.has_value())
                .dump("file", file)
                .dump("entry", entry.entry_point)
                .dump("validator", refused.value_or(cc::string()));
        }
    }
    CC_LOG_INFO("SGL's DXIL lowers {} of the {} compute entry points", lowered, reached);
    // the number the slice reaches, so a lowering lost shows here rather than as a quiet fall back to DXC
    CHECK(lowered >= 60);
}

TEST("slib - DXIL's float flags stand on each operation, and only `fast` lets a sum reorder")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    auto const fast = written(k_sums, "fast_sum");
    auto const strict = written(k_sums, "strict_sum");
    REQUIRE(fast.has_value());
    REQUIRE(strict.has_value());
    auto const fast_text = compiler.value().disassemble(fast.value().dxil);
    auto const strict_text = compiler.value().disassemble(strict.value().dxil);
    REQUIRE(fast_text.has_value());
    REQUIRE(strict_text.has_value());
    CHECK(fast_text.value().contains("fadd fast float"));
    CHECK(!strict_text.value().contains("fast float"));
    CHECK(strict_text.value().contains("fadd float"));
}

TEST("slib - a container SGL writes carries the digest the retail runtime takes unsigned, and its stage's parts")
{
    auto const e = written(k_sums, "fast_sum");
    REQUIRE(e.has_value());
    auto const bytes = cc::span<byte const>(e.value().dxil);
    REQUIRE(bytes.size() > 32);
    CHECK(u8(bytes[0]) == 'D');
    CHECK(u8(bytes[3]) == 'C');
    for (auto i = isize(4); i < 20; ++i)
        CHECK(u8(bytes[i]) == 1);
}
