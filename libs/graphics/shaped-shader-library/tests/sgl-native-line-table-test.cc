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

// EMIT-257: the debug module SGL's DXIL carries beside its program, as DXC's own tools read it.
// No debugger runs here, so DXC's validator and disassembler stand in for PIX: they read the same part.

namespace
{
// line 7 is a statement of an inlined function, and line 12 the call that inlined it
constexpr cc::string_view k_lines = "binding work:\n"                                   // 1
                                    "    values: mut buffer[float]\n"                   // 2
                                    "\n"                                                // 3
                                    "fun clamped(x: float) -> float:\n"                 // 4
                                    "    let mut y = x\n"                               // 5
                                    "    if y > 1.0:\n"                                 // 6
                                    "        y = y * 0.5\n"                             // 7
                                    "    return y\n"                                    // 8
                                    "\n"                                                // 9
                                    "@compute(1) fun run(@thread_id id: int3){work}:\n" // 10
                                    "    let x = work.values[0]\n"                      // 11
                                    "    work.values[1] = clamped(x)\n";                // 12

[[nodiscard]] u32 u32_at(cc::span<byte const> bytes, isize at)
{
    return u32(bytes[at]) | u32(bytes[at + 1]) << 8 | u32(bytes[at + 2]) << 16 | u32(bytes[at + 3]) << 24;
}

/// A container holding only part `name` of `container`, as its program: what DXC's disassembly reads a debug module in.
[[nodiscard]] cc::vector<byte> part_as_program(cc::span<byte const> container, cc::string_view name)
{
    auto body = cc::span<byte const>();
    for (auto k = u32(0); k < u32_at(container, 28); ++k)
    {
        auto const at = isize(u32_at(container, 32 + 4 * isize(k)));
        if (container[at] == byte(name[0]) && container[at + 1] == byte(name[1]) && container[at + 2] == byte(name[2])
            && container[at + 3] == byte(name[3]))
            body = container.subspan({.offset = at + 8, .size = isize(u32_at(container, at + 4))});
    }
    auto out = cc::vector<byte>();
    if (body.empty())
        return out;
    auto const word = [&](u32 v)
    {
        for (auto s = 0; s < 32; s += 8)
            out.push_back(byte(v >> s));
    };
    auto const header = u32(32 + 4);
    out.push_back(byte('D'));
    out.push_back(byte('X'));
    out.push_back(byte('B'));
    out.push_back(byte('C'));
    for (auto i = 0; i < 16; ++i)
        out.push_back(byte(0));
    word(1);
    word(u32(header + 8 + body.size()));
    word(1);
    word(header);
    for (auto const c : cc::string_view("DXIL"))
        out.push_back(byte(c));
    word(u32(body.size()));
    for (auto const b : body)
        out.push_back(b);
    return out;
}

[[nodiscard]] cc::string read_file(cc::string_view path)
{
    auto adapter = cc::file_read_stream_adapter::open(path);
    REQUIRE(adapter.has_value());
    auto stream = adapter.value().stream();
    auto bytes = stream.read_all();
    REQUIRE(bytes.has_value());
    return cc::string(cc::string_view(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size()));
}

[[nodiscard]] cc::vector<cc::string> files_under(cc::string_view root)
{
    namespace fs = std::filesystem;
    auto found = cc::vector<cc::string>();
    for (auto const& e : fs::recursive_directory_iterator(fs::path(root.data(), root.data() + root.size())))
        if (e.is_regular_file() && e.path().extension() == ".sgl")
            found.push_back(cc::string(e.path().generic_string().c_str()));
    cc::sort(found);
    return found;
}
} // namespace

TEST("slib - SGL's DXIL with its line table passes the validator, and DXC reads each .sgl line out of it (EMIT-257)")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    auto const e = sgl::compile_to_text({.source = k_lines,
                                         .source_name = "lines.sgl",
                                         .entry_point = "run",
                                         .target = sgl::emit::target::hlsl_dx12,
                                         .native = sgl::emit::native_route::require,
                                         .native_line_table = true});
    REQUIRE(e.has_value());
    auto const refused = compiler.value().validate(e.value().dxil);
    CHECK(!refused.has_value()).dump("validator", refused.value_or(cc::string()));

    auto const debug = part_as_program(e.value().dxil, "ILDB");
    REQUIRE(!debug.empty());
    auto const text = compiler.value().disassemble(debug);
    REQUIRE(text.has_value()).dump("error", text.has_error() ? text.error().to_string() : cc::string());
    auto const& t = text.value();
    // the entry point's own lines, and the inlined statement in its own scope, inlined at the call
    CHECK(t.contains("; line:11"));
    CHECK(t.contains("; line:12"));
    CHECK(t.contains("fmul fast float"));
    CHECK(t.contains("!DILocation(line: 7, scope:"));
    CHECK(t.contains("inlinedAt:"));
    CHECK(t.contains("distinct !DILocation(line: 12, scope:"));
    CHECK(t.contains("!DIFile(filename: \"lines.sgl\""));
    // the source itself, so PIX shows the line without the file
    CHECK(t.contains("!dx.source.contents"));
    CHECK(t.contains("fun clamped(x: float) -> float:"));
}

TEST("slib - every entry point SGL's DXIL lowers passes the validator with its line table")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    auto files = files_under(nx::source_path("libs/graphics/shaped-graphics/tests/shaders/sgl"));
    files.push_back_range(files_under(nx::source_path("libs/graphics/shaped-graphics/tests/shaders/sgl_mixed")));
    files.push_back_range(files_under(nx::source_path("libs/graphics/shaped-rendering/shaders")));
    REQUIRE(!files.empty());

    auto const stride = nx::is_thorough() ? 1 : 8;
    auto validated = 0;
    auto seen = 0;
    for (auto const& file : files)
    {
        auto const source = read_file(file);
        auto const all = sgl::compile_all_to_text({.source = source, .targets = {sgl::emit::target::hlsl_dx12}});
        if (all.has_error())
            continue;
        for (auto const& entry : all.value())
        {
            if (seen++ % stride != 0)
                continue;
            auto const e = sgl::compile_to_text({.source = source,
                                                 .source_name = file,
                                                 .entry_point = entry.entry_point,
                                                 .target = sgl::emit::target::hlsl_dx12,
                                                 .native = sgl::emit::native_route::prefer,
                                                 .native_line_table = true});
            if (e.has_error() || e.value().dxil.empty())
                continue;
            ++validated;
            auto const refused = compiler.value().validate(e.value().dxil);
            CHECK(!refused.has_value())
                .context(cc::format("{} {}", file, entry.entry_point))
                .dump("validator", refused.value_or(cc::string()));
        }
    }
    CC_LOG_INFO("{} DXIL containers with a line table passed the validator", validated);
    CHECK(validated >= (nx::is_thorough() ? 200 : 20));
}
