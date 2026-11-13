#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/macros.hh> // CC_OS_WINDOWS
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <nexus/source_root.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/check/symbols.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/ir/ir.hh>
#include <shaped-shader-compiler-dxc/compiler.hh>

#include <filesystem>

using namespace cc::primitive_defines;

// DXIL's packer against DXC itself, element by element, because a pair of stages only links if both place an element
// the same way, and a stage of either route may meet a stage of the other.
// This is also the guard on a DXC upgrade: a change of its default packing fails here first.

namespace
{
using sgl::ir::dxil::signature_element;
using sgl::ir::dxil::signature_table;

namespace fs = std::filesystem;

// DXC's compile to DXIL ends in reflection through d3d12shader.h, which the Linux release does not ship.
#ifdef CC_OS_WINDOWS
constexpr bool k_dxc_writes_dxil = true;
#else
constexpr bool k_dxc_writes_dxil = false;
#endif

[[nodiscard]] cc::string read_file(cc::string_view path)
{
    auto adapter = cc::file_read_stream_adapter::open(path);
    REQUIRE(adapter.has_value());
    auto stream = adapter.value().stream();
    auto bytes = stream.read_all();
    REQUIRE(bytes.has_value());
    return cc::string(cc::string_view(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size()));
}

/// One row of a container's `ISG1`, `OSG1` or `PSG1`, with the mode the disassembly states beside it.
struct dxc_element
{
    signature_table table = signature_table::input;
    cc::string name;
    u32 semantic_index = 0;
    u32 system_value = 0;
    u32 component_type = 0;
    u32 row = 0;
    u32 mask = 0;
    /// The lanes an input reads, or the lanes an output never writes, as the final operations settled them.
    u32 usage = 0;
    cc::string mode;
};

template <class T>
[[nodiscard]] T read_at(cc::span<byte const> bytes, isize at)
{
    auto value = T();
    cc::memcpy(&value, bytes.data() + at, sizeof(T));
    return value;
}

/// `DxilProgramSignature`: a count and an offset, then 32 bytes per element.
void read_part(cc::span<byte const> part, signature_table table, cc::vector<dxc_element>& out)
{
    auto const count = read_at<u32>(part, 0);
    auto const first = isize(read_at<u32>(part, 4));
    for (auto i = isize(0); i < isize(count); ++i)
    {
        auto const at = first + i * 32;
        auto const name = cc::string_view(reinterpret_cast<char const*>(part.data()) + read_at<u32>(part, at + 4));
        out.push_back({.table = table,
                       .name = cc::string(name),
                       .semantic_index = read_at<u32>(part, at + 8),
                       .system_value = read_at<u32>(part, at + 12),
                       .component_type = read_at<u32>(part, at + 16),
                       .row = read_at<u32>(part, at + 20),
                       .mask = u32(read_at<u8>(part, at + 24)),
                       .usage = u32(read_at<u8>(part, at + 25))});
    }
}

[[nodiscard]] cc::vector<dxc_element> signatures_of(cc::span<byte const> container)
{
    auto result = cc::vector<dxc_element>();
    auto const parts = isize(read_at<u32>(container, 28));
    for (auto i = isize(0); i < parts; ++i)
    {
        auto const at = isize(read_at<u32>(container, 32 + i * 4));
        auto const size = isize(read_at<u32>(container, at + 4));
        auto const part = cc::span<byte const>(container.data() + at + 8, size);
        auto const fourcc = cc::string_view(reinterpret_cast<char const*>(container.data()) + at, 4);
        if (fourcc == "ISG1")
            read_part(part, signature_table::input, result);
        else if (fourcc == "OSG1")
            read_part(part, signature_table::output, result);
        else if (fourcc == "PSG1")
            read_part(part, signature_table::patch, result);
    }
    return result;
}

/// The disassembly's second table of each signature, `Name Index InterpMode DynIdx`, onto `elements`.
void read_modes(cc::string_view text, cc::vector<dxc_element>& elements)
{
    auto table = signature_table::input;
    auto is_reading = false;
    auto at = isize(0);
    while (at < text.size())
    {
        auto end = at;
        while (end < text.size() && text[end] != '\n')
            ++end;
        auto line = text.subview({.offset = at, .size = end - at});
        at = end + 1;
        if (line.size() < 2 || line[0] != ';')
        {
            if (line.empty() || line[0] != ';')
                break;
            is_reading = false;
            continue;
        }
        line = line.subview({.offset = 2, .size = line.size() - 2});
        if (line.starts_with("Input signature"))
            table = signature_table::input;
        else if (line.starts_with("Output signature") || line.starts_with("Vertex Output signature"))
            table = signature_table::output;
        else if (line.starts_with("Patch Constant signature") || line.starts_with("Primitive Output signature"))
            table = signature_table::patch;
        if (line.starts_with("Name ") && line.contains("InterpMode"))
        {
            is_reading = true;
            continue;
        }
        if (line.starts_with("---"))
            continue;
        if (!is_reading)
            continue;
        if (line.empty())
        {
            is_reading = false;
            continue;
        }
        auto words = cc::vector<cc::string_view>();
        auto word = isize(0);
        for (auto i = isize(0); i <= line.size(); ++i)
            if (i == line.size() || line[i] == ' ' || line[i] == '\r')
            {
                if (i > word)
                    words.push_back(line.subview({.offset = word, .size = i - word}));
                word = i + 1;
            }
        if (words.size() < 2)
            continue;
        auto index = u32(0);
        for (auto const c : words[1])
            index = index * 10 + u32(c - '0');
        auto mode = cc::string();
        for (auto i = isize(2); i < words.size(); ++i)
            mode.appendf("{}{}", mode.empty() ? "" : " ", words[i]);
        for (auto& e : elements)
            if (e.table == table && e.name == words[0] && e.semantic_index == index)
                e.mode = mode;
    }
}

[[nodiscard]] cc::string_view mode_text(sgl::ir::interpolation mode)
{
    using sgl::ir::interpolation;
    switch (mode)
    {
    case interpolation::none:
        return "";
    case interpolation::perspective:
        return "linear";
    case interpolation::centroid:
        return "centroid";
    case interpolation::sample:
        return "sample";
    case interpolation::noperspective:
        return "noperspective";
    case interpolation::noperspective_centroid:
        return "noperspective centroid";
    case interpolation::noperspective_sample:
        return "noperspective sample";
    case interpolation::flat:
        return "nointerpolation";
    }
    return "";
}

[[nodiscard]] cc::optional<sg::shader_stage> shader_stage_of(sgl::check::stage s)
{
    using sgl::check::stage;
    switch (s)
    {
    case stage::vertex:
        return sg::shader_stage::vertex;
    case stage::pixel:
        return sg::shader_stage::fragment;
    case stage::geometry:
        return sg::shader_stage::geometry;
    case stage::tessellation_control:
        return sg::shader_stage::tessellation_control;
    case stage::tessellation_evaluation:
        return sg::shader_stage::tessellation_evaluation;
    case stage::mesh:
        return sg::shader_stage::mesh;
    default:
        return {};
    }
}

[[nodiscard]] cc::string_view table_name(signature_table t)
{
    return t == signature_table::input ? "input" : t == signature_table::output ? "output" : "patch";
}

/// What differs between ours and DXC's, one line each; an element on one side alone is a line too.
[[nodiscard]] cc::vector<cc::string> differences(cc::span<signature_element const> ours,
                                                 cc::span<dxc_element const> theirs)
{
    auto result = cc::vector<cc::string>();
    for (auto const& d : theirs)
    {
        signature_element const* found = nullptr;
        for (auto const& o : ours)
            if (o.table == d.table && o.name == d.name && o.semantic_index == d.semantic_index)
                found = &o;
        auto const where = cc::format("{} {}{}", table_name(d.table), d.name, d.semantic_index);
        if (found == nullptr)
        {
            result.push_back(cc::format("{}: DXC alone, row {} mask {}", where, i32(d.row), d.mask));
            continue;
        }
        if (found->row != i32(d.row))
            result.push_back(cc::format("{}: row {} against DXC's {}", where, found->row, i32(d.row)));
        else if (found->row >= 0 && found->mask() != d.mask)
            result.push_back(cc::format("{}: mask {} against DXC's {}", where, found->mask(), d.mask));
        if (found->system_value != d.system_value)
            result.push_back(
                cc::format("{}: system value {} against DXC's {}", where, found->system_value, d.system_value));
        if (found->component_type != d.component_type)
            result.push_back(
                cc::format("{}: component {} against DXC's {}", where, found->component_type, d.component_type));
        if (mode_text(found->mode) != d.mode)
            result.push_back(cc::format("{}: mode '{}' against DXC's '{}'", where, mode_text(found->mode), d.mode));
    }
    for (auto const& o : ours)
    {
        auto is_there = false;
        for (auto const& d : theirs)
            is_there = is_there || (o.table == d.table && o.name == d.name && o.semantic_index == d.semantic_index);
        if (!is_there)
            result.push_back(cc::format("{} {}{}: ours alone", table_name(o.table), o.name, o.semantic_index));
    }
    return result;
}

struct sweep_counts
{
    i32 entries = 0;
    /// Those SGL's DXIL wrote too, whose usage masks the comparison reaches.
    i32 lowered = 0;
    i32 no_dxc = 0;
    i32 elements = 0;
    i32 stages[8] = {};
    i32 front_end_failures = 0;
    cc::string first_failure;
    cc::vector<cc::string> mismatches;
};

/// Compares one entry point, as it stands in a compile `request` asks for, against DXC's build of its text.
void compare_entry(ssc::dxc::compiler& compiler, sgl::text_request request, sweep_counts& counts, cc::string_view label)
{
    request.target = sgl::emit::target::hlsl_dx12;
    request.native = sgl::emit::native_route::prefer;
    auto const emitted = sgl::compile_to_text(request);
    if (emitted.has_error())
        return;
    auto const stage = shader_stage_of(emitted.value().stage);
    if (!stage.has_value())
        return;
    // a stage that lowers holds its program where the text would stand, and DXC wants the text
    request.native = sgl::emit::native_route::text;
    auto const text_route = sgl::compile_to_text(request);
    if (text_route.has_error())
        return;
    auto const built = compiler.compile(
        {.source = text_route.value().text, .entry_point = text_route.value().entry_point, .stage = stage.value()},
        {.target = ssc::dxc::compile_target::dxil,
         .warnings_as_errors = false,
         .extra_args = {cc::string("-enable-16bit-types")}});
    if (built.has_error())
    {
        ++counts.no_dxc;
        counts.first_failure = cc::format("{}", built.error());
        return;
    }
    auto const bytes = cc::span<byte const>(built.value().bytecode.data(), built.value().bytecode.size());
    auto theirs = signatures_of(bytes);
    auto const text = compiler.disassemble(bytes);
    REQUIRE(text.has_value());
    read_modes(text.value(), theirs);
    ++counts.entries;
    ++counts.stages[isize(emitted.value().stage) % 8];
    counts.elements += i32(theirs.size());
    for (auto& line : differences(emitted.value().signature, theirs))
        counts.mismatches.push_back(cc::format("{} {}: {}", label, request.entry_point, line));

    // A lowered stage's own container: every output lane written as DXC writes it, and every lane DXC reads read.
    // Ours may read more, since a struct an index reaches stays in memory, and a read no one can see links as well.
    if (emitted.value().dxil.empty())
        return;
    ++counts.lowered;
    for (auto const& o : signatures_of(emitted.value().dxil))
        for (auto const& d : theirs)
        {
            if (o.table != d.table || o.name != d.name || o.semantic_index != d.semantic_index)
                continue;
            auto const agrees = o.table == signature_table::input ? (d.usage & ~o.usage) == 0 : d.usage == o.usage;
            if (!agrees)
                counts.mismatches.push_back(cc::format("{} {}: {}{} used {} against DXC's {}", label,
                                                       request.entry_point, o.name, o.semantic_index, o.usage, d.usage));
        }
}

void report(sweep_counts const& counts)
{
    for (auto const& line : counts.mismatches)
        CC_LOG_INFO("{}", line);
    CC_LOG_INFO("{} entry points, {} of them lowered, {} elements, {} refused by DXC, {} refused by SGL, {} "
                "mismatches; {}",
                counts.entries, counts.lowered, counts.elements, counts.no_dxc, counts.front_end_failures,
                counts.mismatches.size(), counts.first_failure);
}
[[nodiscard]] cc::vector<fs::path> sources_under(fs::path const& root)
{
    auto found = cc::vector<fs::path>();
    for (auto const& e : fs::recursive_directory_iterator(root))
        if (e.is_regular_file() && e.path().extension() == ".sgl")
            found.push_back(e.path());
    return found;
}
} // namespace

TEST("sgl signature - DXIL's packer places every element of the repo's SGL as DXC does",
     nx::config::rerun_on("libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/dxil/signature.cc"))
{
    if constexpr (!k_dxc_writes_dxil)
        SKIP("DXC writes DXIL on Windows alone");
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");

    auto const here = nx::source_path(SLIB_SOURCE_DIR);
    auto const root = (fs::path(here.data(), here.data() + here.size()) / ".." / ".." / "..").lexically_normal();
    auto files = sources_under(root / "libs");
    for (auto const& f : sources_under(root / "examples"))
        files.push_back(f);

    // a shader of these directories may `use` a module of its neighbours', or slug's
    auto library_of = [&](fs::path const& directory, cc::vector<cc::string>& names, cc::vector<cc::string>& sources)
    {
        for (auto const& d : {directory, root / "libs/graphics/shaped-rendering/shaders"})
            for (auto const& e : fs::directory_iterator(d))
                if (e.is_regular_file() && e.path().extension() == ".sgl")
                {
                    names.push_back(cc::string(e.path().generic_string().c_str()));
                    sources.push_back(read_file(names.back()));
                }
    };

    auto counts = sweep_counts();
    for (auto const& file : files)
    {
        auto const path = cc::string(file.generic_string().c_str());
        auto const source = read_file(path);
        auto names = cc::vector<cc::string>();
        auto sources = cc::vector<cc::string>();
        library_of(file.parent_path(), names, sources);
        auto library = cc::vector<sgl::library_file>();
        for (auto i = isize(0); i < names.size(); ++i)
            library.push_back({.name = names[i], .source = sources[i]});
        sgl::emit::target const targets[] = {sgl::emit::target::hlsl_dx12};
        auto const all
            = sgl::compile_all_to_text({.source = source, .source_name = path, .library = library, .targets = targets});
        if (all.has_error())
            continue;
        for (auto const& entry : all.value())
            if (entry.text.has_value())
                compare_entry(
                    compiler.value(),
                    {.source = source, .source_name = path, .library = library, .entry_point = entry.entry_point},
                    counts, path);
    }

    report(counts);
    CHECK(counts.mismatches.empty());
    // a sweep that quietly lost its shaders would pass the check above
    CHECK(counts.entries >= 150);
    CHECK(counts.elements >= 400);
}

namespace
{
constexpr cc::string_view k_stage_heads = "require geometry_shader\n"
                                          "require tessellation_shader\n"
                                          "require view_instancing\n"
                                          "require first_instance\n"
                                          "require primitive_index\n"
                                          "require sample_rate_shading\n"
                                          "require vertex_layer_output\n"
                                          "struct point:\n"
                                          "    position: float2\n"
                                          "    @interpolate(.flat) tag: int\n"
                                          "struct out_link:\n"
                                          "    @position position: hpos4\n"
                                          "    color: float3\n"
                                          "    alpha: float\n";

/// What no shader of the repo declares and the packing still has to hold.
constexpr cc::string_view k_cases[] = {
    // links beside the factors
    "struct more:\n"
    "    @edge_factors edges: float[3]\n"
    "    @inside_factors inside: float\n"
    "    extra: float3\n"
    "    tag: float\n"
    "    wide: float4\n"
    "@tessellation_control(partitioning = .integer, winding = .counter_clockwise)\n"
    "fun more_tc(patch: point[3]) -> more:\n"
    "    return { edges = [1.0, 1.0, 1.0], inside = 1.0, extra = float3(0.0, 0.0, 0.0), tag = 0.0, wide = float4(0.0, "
    "0.0, 0.0, 0.0) }\n"
    "@tessellation_evaluation fun more_te(patch: point[3], f: more, @domain_location uvw: float3) -> out_link:\n"
    "    return { position = hpos4(f.extra.x, f.tag, f.wide.x, 1.0), color = uvw, alpha = 1.0 }\n",
    "struct quad_more:\n"
    "    extra: float2\n"
    "    @edge_factors edges: float[4]\n"
    "    @inside_factors inside: float[2]\n"
    "    tag: float3\n"
    "@tessellation_control(partitioning = .integer, winding = .counter_clockwise)\n"
    "fun quad_tc(patch: point[4]) -> quad_more:\n"
    "    return { extra = float2(0.0, 0.0), edges = [1.0, 1.0, 1.0, 1.0], inside = [1.0, 1.0], tag = float3(0.0, 0.0, "
    "0.0) }\n"
    "@tessellation_evaluation fun quad_te(patch: point[4], f: quad_more, @domain_location uv: float2) -> out_link:\n"
    "    return { position = hpos4(f.extra.x, f.tag.x, 0.0, 1.0), color = float3(uv.x, uv.y, 0.0), alpha = 1.0 }\n",
    "struct line_factors:\n"
    "    @edge_factors edges: float[2]\n"
    "    extra: float3\n"
    "@tessellation_control(partitioning = .integer, winding = .counter_clockwise)\n"
    "fun line_tc(patch: point[2]) -> line_factors:\n"
    "    return { edges = [1.0, 1.0], extra = float3(0.0, 0.0, 0.0) }\n"
    "@tessellation_evaluation fun line_te(patch: point[2], f: line_factors, @domain_location uv: float2) -> out_link:\n"
    "    return { position = hpos4(uv.x, uv.y, f.extra.x, 1.0), color = float3(0.0, 0.0, 0.0), alpha = 1.0 }\n",
    // the system values each stage may take
    "struct tri_factors:\n"
    "    @edge_factors edges: float[3]\n"
    "    @inside_factors inside: float\n"
    "@tessellation_control(partitioning = .integer, winding = .counter_clockwise)\n"
    "fun id_tc(patch: point[3], @primitive_id id: int) -> tri_factors:\n"
    "    return { edges = [id as float, 1.0, 1.0], inside = 1.0 }\n"
    "@tessellation_evaluation fun id_te(patch: point[3], f: tri_factors, @domain_location uvw: float3, @primitive_id "
    "id: int) -> out_link:\n"
    "    return { position = hpos4(uvw.x, uvw.y, id as float, 1.0), color = uvw, alpha = 1.0 }\n",
    "@vertex struct corner:\n"
    "    position: float2\n"
    "@vertex fun indices_vs(v: corner, @vertex_index vertex: int, @instance_index instance: int, @first_instance "
    "first: int) -> out_link:\n"
    "    return { position = hpos4(v.position.x, v.position.y, vertex as float, (instance + first) as float), color = "
    "float3(0.0, 0.0, 0.0), alpha = 1.0 }\n"
    "@vertex fun view_vs(v: corner, @view_index view: int) -> out_link:\n"
    "    return { position = hpos4(v.position.x, view as float, 0.0, 1.0), color = float3(0.0, 0.0, 0.0), alpha = 1.0 "
    "}\n",
    "@pixel fun all_ps(p: out_link, @is_front_facing front: bool, @primitive_id id: int, @sample_index s: int, "
    "@sample_mask m: uint, @view_index view: int) -> float4:\n"
    "    if front:\n"
    "        return float4(p.color.x, (id + s + view) as float, m as float, 1.0)\n"
    "    return float4(p.color.x, 0.0, 0.0, 0.0)\n",
    "struct gs_link:\n"
    "    @position position: hpos4\n"
    "    @layer layer: uint\n"
    "    color: float3\n"
    "@geometry(max_vertices = 3)\n"
    "fun view_gs(tri: out_link[3], @primitive_id id: int, stream: mut triangle_stream[gs_link]):\n"
    "    for i in 0 ..< 3:\n"
    "        stream.emit({ position = tri[i].position, layer = id as uint, color = tri[i].color })\n"
    "    stream.end_strip()\n",
};

/// A small repeatable stream, so a failing link can be run again from its seed.
struct random_stream
{
    u64 state = 0;

    [[nodiscard]] u32 next(u32 below)
    {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return u32(state >> 40) % below;
    }
};

/// A stage link of random members in SGL, with what each stage may read beside it, as a vertex and a pixel entry point.
[[nodiscard]] cc::string random_link(random_stream& random)
{
    constexpr cc::string_view modes[] = {"",
                                         "",
                                         "@interpolate(.linear) ",
                                         "@interpolate(.perspective, .centroid) ",
                                         "@interpolate(.perspective, .sample) ",
                                         "@interpolate(.linear, .centroid) ",
                                         "@interpolate(.linear, .sample) ",
                                         "@interpolate(.flat) "};
    constexpr cc::string_view floats[] = {"float", "float2", "float3", "float4", "float[2]", "float[3]"};
    constexpr cc::string_view zeros[]
        = {"0.0",        "float2(0.0, 0.0)", "float3(0.0, 0.0, 0.0)", "float4(0.0, 0.0, 0.0, 0.0)",
           "[0.0, 0.0]", "[0.0, 0.0, 0.0]"};
    constexpr cc::string_view ints[] = {"int", "int2", "int3", "uint", "uint2", "int[2]"};
    constexpr cc::string_view int_zeros[]
        = {"0", "int2(0, 0)", "int3(0, 0, 0)", "0 as uint", "uint2(0 as uint, 0 as uint)", "[0, 0]"};
    auto const members = random.next(12) + 1;
    auto const position_at = random.next(members + 1);
    auto const layer_at = random.next(2) == 0 ? random.next(members + 1) : members + 1;
    auto fields = cc::string();
    auto values = cc::string();
    for (auto i = u32(0); i <= members; ++i)
    {
        if (i == position_at)
        {
            fields += "    @position position: hpos4\n";
            values += "        position = hpos4(0.0, 0.0, 0.0, 1.0)\n";
        }
        if (i == layer_at)
        {
            fields += "    @layer layer: uint\n";
            values += "        layer = 0 as uint\n";
        }
        if (i == members)
            break;
        auto const mode = modes[random.next(8)];
        if (mode.contains(".flat") && random.next(2) == 0)
        {
            auto const kind = random.next(6);
            fields.appendf("    {}m{}: {}\n", mode, i, ints[kind]);
            values.appendf("        m{} = {}\n", i, int_zeros[kind]);
        }
        else
        {
            auto const kind = random.next(6);
            fields.appendf("    {}m{}: {}\n", mode, i, floats[kind]);
            values.appendf("        m{} = {}\n", i, zeros[kind]);
        }
    }
    constexpr cc::string_view inputs[]
        = {", @is_front_facing front: bool", ", @primitive_id id: int", ", @sample_index s: int"};
    auto tail = cc::string();
    auto const first = random.next(3);
    for (auto i = u32(0); i < 3; ++i)
        if (random.next(2) == 0)
            tail += inputs[(first + i) % 3];
    return cc::format("require primitive_index\nrequire sample_rate_shading\nrequire vertex_layer_output\n"
                      "struct link:\n{}\n@vertex fun link_vs() -> link:\n    return {{\n{}    }}\n\n"
                      "@pixel fun link_ps(v: link{}) -> float4:\n    return float4(1.0, 1.0, 1.0, 1.0)\n",
                      fields, values, tail);
}

void compare_source(ssc::dxc::compiler& compiler, cc::string_view source, sweep_counts& counts, cc::string_view label)
{
    sgl::emit::target const targets[] = {sgl::emit::target::hlsl_dx12};
    auto const all = sgl::compile_all_to_text({.source = source, .targets = targets});
    if (all.has_error())
    {
        ++counts.front_end_failures;
        counts.first_failure = all.error();
        return;
    }
    for (auto const& entry : all.value())
        if (entry.text.has_value())
            compare_entry(compiler, {.source = source, .entry_point = entry.entry_point}, counts, label);
        else
        {
            ++counts.front_end_failures;
            counts.first_failure = entry.text.error();
        }
}

} // namespace

TEST("sgl signature - the places no shader of the repo declares match DXC too",
     nx::config::rerun_on("libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/dxil/signature.cc"))
{
    if constexpr (!k_dxc_writes_dxil)
        SKIP("DXC writes DXIL on Windows alone");
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    auto counts = sweep_counts();
    auto index = 0;
    for (auto const c : k_cases)
    {
        compare_source(compiler.value(), cc::format("{}{}", k_stage_heads, c), counts, cc::format("case {}", index));
        ++index;
    }
    report(counts);
    CHECK(counts.mismatches.empty());
    CHECK(counts.no_dxc == 0);
    CHECK(counts.front_end_failures == 0);
}

TEST("sgl signature - an element placed apart from DXC is caught, by this comparison and by the validator",
     nx::config::rerun_on("libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/dxil/signature.cc"))
{
    if constexpr (!k_dxc_writes_dxil)
        SKIP("DXC writes DXIL on Windows alone");
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    // `tail` shares `near`'s row, so moving it is the mistake a packer that took one row per location makes
    constexpr cc::string_view source
        = "struct shared_row:\n"
          "    @position position: hpos4\n"
          "    near: float3\n"
          "    tail: float\n"
          "@vertex fun shared_vs(@vertex_index v: int) -> shared_row:\n"
          "    let x = v as float\n"
          "    return { position = hpos4(x, x, x, 1.0), near = float3(x, x, x), tail = x }\n";
    auto request = sgl::text_request{.source = source,
                                     .entry_point = "shared_vs",
                                     .target = sgl::emit::target::hlsl_dx12,
                                     .native = sgl::emit::native_route::require};
    auto const emitted = sgl::compile_to_text(request);
    REQUIRE(emitted.has_value());
    REQUIRE(!emitted.value().dxil.empty());
    request.native = sgl::emit::native_route::text;
    auto const text = sgl::compile_to_text(request);
    REQUIRE(text.has_value());
    auto const built = compiler.value().compile(
        {.source = text.value().text, .entry_point = "shared_vs", .stage = sg::shader_stage::vertex},
        {.target = ssc::dxc::compile_target::dxil,
         .warnings_as_errors = false,
         .extra_args = {cc::string("-enable-16bit-types")}});
    REQUIRE(built.has_value());
    auto const bytes = cc::span<byte const>(built.value().bytecode.data(), built.value().bytecode.size());
    auto theirs = signatures_of(bytes);
    auto const disassembly = compiler.value().disassemble(bytes);
    REQUIRE(disassembly.has_value());
    read_modes(disassembly.value(), theirs);
    CHECK(differences(emitted.value().signature, theirs).empty());

    // this comparison: the element a row down, then in the columns it left free, then in another mode
    auto const with = [&](auto&& change)
    {
        auto moved = cc::vector<signature_element>();
        for (auto const& e : emitted.value().signature)
        {
            moved.push_back(e);
            if (moved.back().table == signature_table::output && moved.back().semantic_index == 1)
                change(moved.back());
        }
        return differences(moved, theirs);
    };
    auto const rowed = with([](signature_element& e) { ++e.row; });
    REQUIRE(rowed.size() == 1);
    CHECK(rowed[0].contains("row"));
    auto const columned = with([](signature_element& e) { e.first_column = 0; });
    REQUIRE(columned.size() == 1);
    CHECK(columned[0].contains("mask"));
    auto const moded = with([](signature_element& e) { e.mode = sgl::ir::interpolation::flat; });
    REQUIRE(moded.size() == 1);
    CHECK(moded[0].contains("mode"));

    // the validator: the same move in a container's own table, which `dx.sig` and `PSV0` then disagree with
    CHECK(!compiler.value().validate(emitted.value().dxil).has_value());
    auto mispacked = cc::vector<byte>::create_copy_of(cc::span<byte const>(emitted.value().dxil));
    auto const parts = isize(read_at<u32>(mispacked, 28));
    auto moved_row = false;
    for (auto i = isize(0); i < parts; ++i)
    {
        auto const at = isize(read_at<u32>(mispacked, 32 + i * 4));
        if (cc::string_view(reinterpret_cast<char const*>(mispacked.data()) + at, 4) != "OSG1")
            continue;
        auto const part = at + 8;
        auto const first = part + isize(read_at<u32>(mispacked, part + 4));
        for (auto e = isize(0); e < isize(read_at<u32>(mispacked, part)); ++e)
        {
            auto const row_at = first + e * 32 + 20;
            if (read_at<u32>(mispacked, first + e * 32 + 8) != 1)
                continue;
            auto const row = read_at<u32>(mispacked, row_at) + 1;
            cc::memcpy(mispacked.data() + row_at, &row, sizeof(row));
            moved_row = true;
        }
    }
    REQUIRE(moved_row);
    CHECK(compiler.value().validate(mispacked).has_value());
}

TEST("sgl signature - random stage links match DXC, and each side of a link agrees",
     nx::config::rerun_on("libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/dxil/signature.cc"))
{
    if constexpr (!k_dxc_writes_dxil)
        SKIP("DXC writes DXIL on Windows alone");
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    auto counts = sweep_counts();
    auto const links = nx::is_thorough() ? 250 : 30;
    for (auto seed = 0; seed < links; ++seed)
    {
        auto random = random_stream{.state = u64(seed) + 1};
        auto const source = random_link(random);
        compare_source(compiler.value(), source, counts, cc::format("seed {}", seed));

        auto const vs = sgl::compile_to_text({.source = source,
                                              .entry_point = "link_vs",
                                              .target = sgl::emit::target::hlsl_dx12,
                                              .native = sgl::emit::native_route::prefer});
        auto const ps = sgl::compile_to_text({.source = source,
                                              .entry_point = "link_ps",
                                              .target = sgl::emit::target::hlsl_dx12,
                                              .native = sgl::emit::native_route::prefer});
        if (vs.has_error() || ps.has_error())
            continue;
        for (auto const& out : vs.value().signature)
            for (auto const& in : ps.value().signature)
                if (out.table == signature_table::output && in.table == signature_table::input && out.name == in.name
                    && out.semantic_index == in.semantic_index)
                {
                    CHECK(out.row == in.row);
                    CHECK(out.first_column == in.first_column);
                }
    }
    report(counts);
    CHECK(counts.mismatches.empty());
    CHECK(counts.front_end_failures == 0);
    CHECK(counts.entries >= links);
}
