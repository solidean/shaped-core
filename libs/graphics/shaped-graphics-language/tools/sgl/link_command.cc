#include "files.hh"
#include "module_dirs.hh"

#include <babel-data/data/json.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/print.hh>
#include <nexus/args/ambient.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/check/symbols.hh>
#include <shaped-graphics-language/driver/link.hh>
#include <shaped-graphics-language/emit/emit.hh>

using namespace cc::primitive_defines;

namespace
{
// 0 = the map and every binary written; 1 = bad usage or an IO error; 2 = the group does not link, or an entry does not
// emit, which was printed.
constexpr int exit_ok = 0;
constexpr int exit_usage = 1;
constexpr int exit_errors = 2;

/// One entry of a group file, its strings owned, since a `link_entry` only views them.
struct group_row
{
    cc::string file;
    cc::string entry;
    sgl::check::stage stage = sgl::check::stage::none;
    cc::vector<sgl::check::option_value> options;
};

/// `raygen`, `closest_hit`, ... as `check::stage_name` spells them; `none` for a name no stage has.
sgl::check::stage stage_named(cc::string_view name)
{
    for (auto i = u8(0); i <= u8(sgl::check::stage::callable); ++i)
        if (sgl::check::stage_name(sgl::check::stage(i)) == name)
            return sgl::check::stage(i);
    return sgl::check::stage::none;
}

/// The file's extension for what `target` writes.
cc::string_view extension_of(cc::string_view target)
{
    if (target == "dxil")
        return "dxil";
    if (target == "spirv")
        return "spv";
    if (target == "wgsl")
        return "wgsl";
    if (target == "msl")
        return "metal";
    return "hlsl";
}

/// The bytes of `path`, written whole.
bool write_bytes(cc::string_view path, cc::span<byte const> bytes)
{
    auto written = cc::file_write_stream_adapter::create(path);
    if (written.has_error())
        return false;
    auto stream = written.value().stream();
    if (stream.write(bytes).has_error())
        return false;
    return !stream.flush().has_error();
}
} // namespace

// `sgl link`: a group of entries numbered as one pipeline (LINK-n), its map and one output per entry.
COMMAND("link")
{
    auto path = cc::string();
    auto map_path = cc::string();
    auto out_dir = cc::string();
    auto target_name = cc::string("dxil");
    auto module_dirs = cc::vector<cc::string>();
    auto args = nx::args({.name = "sgl link",
                          .description = "Links a group of entry points as one pipeline: numbers the samplers and "
                                         "callables tables they reach once, writes the group's map as JSON and each "
                                         "entry for one target with that numbering."});
    args.positional("GROUP", path,
                    {.desc = "the group, as JSON: {\"name\": \"...\", \"entries\": [{\"file\": \"dir/a.sgl\", "
                             "\"entry\": "
                             "\"raygen\", \"stage\": \"raygen\", \"options\": {\"name\": \"value\"}}]}, files named as "
                             "the module dirs name them"});
    args.arg({"module-dir"}, module_dirs,
             {.desc = "a directory whose .sgl files the group's files are, and the modules they use", .metavar = "DIR"});
    args.arg({"target"}, target_name,
             {.desc = "what each entry is written as: dxil, spirv, hlsl-dx12, hlsl-vulkan, wgsl or msl",
              .metavar = "TARGET"});
    args.arg({"map"}, map_path, {.desc = "write the map here instead of to stdout", .metavar = "PATH"});
    args.arg({"out"}, out_dir,
             {.desc = "write entry i as <DIR>/<i>_<entry>.<ext>; without it, only the map is written", .metavar = "DIR"});
    if (auto const r = args.parse(nx::test_args()); r.should_exit())
        return r.exit_code();

    auto const library = sgl_tool::read_module_dirs(module_dirs);
    if (library.has_error())
    {
        cc::eprintln("sgl link: {}", library.error());
        return exit_usage;
    }
    auto const text = sgl_tool::read_file(path);
    if (text.has_error())
    {
        cc::eprintln("sgl link: cannot read {}: {}", path, text.error());
        return exit_usage;
    }
    auto const document = babel::json::read(cc::string_view(text.value()));
    if (document.has_error())
    {
        cc::eprintln("sgl link: {} is no JSON", path);
        return exit_usage;
    }
    auto const root = document.value().root();
    auto const name = root["name"].as_string(path);
    auto const listed = root["entries"];
    if (!listed.is_array())
    {
        cc::eprintln("sgl link: {} lists no \"entries\"", path);
        return exit_usage;
    }
    auto rows = cc::vector<group_row>();
    for (auto i = isize(0); i < listed.size(); ++i)
    {
        auto const row = listed[i];
        auto made = group_row{.file = cc::string(row["file"].as_string()), .entry = cc::string(row["entry"].as_string())};
        if (made.file.empty() || made.entry.empty())
        {
            cc::eprintln("sgl link: entry {} of {} names no \"file\" or no \"entry\"", i, path);
            return exit_usage;
        }
        if (row.has("stage"))
        {
            made.stage = stage_named(row["stage"].as_string());
            if (made.stage == sgl::check::stage::none)
            {
                cc::eprintln("sgl link: entry {} of {}: unknown stage '{}'", i, path, row["stage"].as_string());
                return exit_usage;
            }
        }
        auto const options = row["options"];
        for (auto k = isize(0); k < options.size(); ++k)
            made.options.push_back({.name = cc::string(options[k].key()), .value = cc::string(options[k].as_string())});
        rows.push_back(cc::move(made));
    }

    // SPIR-V is vulkan's and DXIL dx12's, each written by SGL's own layer
    auto const is_spirv = target_name == "spirv";
    auto const is_dxil = target_name == "dxil";
    auto const text_target = is_spirv ? cc::string_view("hlsl-vulkan")
                           : is_dxil  ? cc::string_view("hlsl-dx12")
                                      : cc::string_view(target_name);
    auto target = sgl::emit::target::hlsl_dx12;
    auto is_known = false;
    for (auto const t : sgl::emit::all_targets())
        if (sgl::emit::to_string(t) == text_target)
        {
            target = t;
            is_known = true;
        }
    if (!is_known)
    {
        cc::eprintln("sgl link: unknown target '{}' (one of dxil, spirv, hlsl-dx12, hlsl-vulkan, wgsl, msl)",
                     target_name);
        return exit_usage;
    }

    auto entries = cc::vector<sgl::link_entry>();
    for (auto const& r : rows)
        entries.push_back({.source_name = r.file, .entry_point = r.entry, .stage = r.stage, .options = r.options});
    sgl::emit::target const targets[] = {target};
    auto const linked = sgl::link({.name = name,
                                   .entries = entries,
                                   .library = library.value().files,
                                   .targets = out_dir.empty() ? cc::span<sgl::emit::target const>() : targets});
    if (linked.has_error())
    {
        cc::eprint(linked.error());
        cc::flush();
        return exit_errors;
    }
    auto const& result = linked.value();
    auto const json = sgl::to_json(result.map());
    if (map_path.empty())
        cc::print(json);
    else if (sgl_tool::write_file(map_path, json).has_error())
    {
        cc::eprintln("sgl link: cannot write {}", map_path);
        return exit_usage;
    }
    if (out_dir.empty())
        return exit_ok;

    auto failed = false;
    for (auto i = isize(0); i < result.entry_count(); ++i)
    {
        auto const emitted = result.emit(
            i, target,
            {.native = is_spirv || is_dxil ? sgl::emit::native_route::require : sgl::emit::native_route::text});
        if (emitted.has_error())
        {
            cc::eprint(emitted.error());
            failed = true;
            continue;
        }
        auto const& source = emitted.value();
        auto const file = cc::format("{}/{}_{}.{}", out_dir, i, rows[i].entry, extension_of(target_name));
        auto const bytes = is_spirv ? cc::span<byte const>(reinterpret_cast<byte const*>(source.spirv.data()),
                                                           source.spirv.size() * 4)
                         : is_dxil  ? cc::span<byte const>(source.dxil)
                                    : cc::as_bytes(cc::string_view(source.text));
        if (!write_bytes(file, bytes))
        {
            cc::eprintln("sgl link: cannot write {}", file);
            return exit_usage;
        }
        // what a cache keys the output on beside the source (LINK-7)
        if (!map_path.empty())
            cc::println("{} {:016x} {}", file, result.numbering_hash(i), result.entry_name(i));
    }
    cc::flush();
    return failed ? exit_errors : exit_ok;
}
