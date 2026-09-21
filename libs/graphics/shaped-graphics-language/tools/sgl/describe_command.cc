#include "files.hh"

#include <babel-data/data/json.hh>
#include <clean-core/string/print.hh>
#include <nexus/args/ambient.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/describe.hh>

using namespace cc::primitive_defines;

namespace
{
// 0 = the description was written, 1 = bad usage or an IO error, 2 = the source has errors, which were printed.
constexpr int exit_ok = 0;
constexpr int exit_usage = 1;
constexpr int exit_errors = 2;

cc::string_view stage_name(sgl::check::stage s)
{
    switch (s)
    {
    case sgl::check::stage::none:
        return "none";
    case sgl::check::stage::vertex:
        return "vertex";
    case sgl::check::stage::pixel:
        return "pixel";
    case sgl::check::stage::compute:
        return "compute";
    }
    return "none";
}

void write_binding(babel::json::object_writer& o, sgl::described_binding const& b)
{
    o.write("name", cc::string_view(b.name));
    o.write("inline", b.is_inline);
    o.write("block_size", b.block_size);
    if (b.block_slot >= 0)
    {
        o.write("block_slot", b.block_slot);
        o.write("block_reflected_name", cc::string_view(b.block_reflected_name));
    }
    auto members = o.write_array("members");
    for (auto const& m : b.members)
    {
        auto mo = members.write_object(babel::json::layout::compact);
        mo.write("name", cc::string_view(m.name));
        mo.write("kind", m.kind == sgl::described_member_kind::buffer ? "buffer" : "constant");
        mo.write("type", cc::string_view(m.type));
        if (m.kind == sgl::described_member_kind::buffer)
        {
            mo.write("mut", m.is_mut);
            mo.write("slot", m.slot);
            mo.write("reflected_name", cc::string_view(m.reflected_name));
        }
        else
        {
            mo.write("offset", m.offset);
            mo.write("size", m.size);
        }
    }
}

void write_struct(babel::json::object_writer& o, sgl::described_struct const& s)
{
    o.write("name", cc::string_view(s.name));
    o.write("edge", stage_name(s.edge));
    auto members = o.write_array("members");
    for (auto const& m : s.members)
    {
        auto mo = members.write_object(babel::json::layout::compact);
        mo.write("name", cc::string_view(m.name));
        mo.write("type", cc::string_view(m.type));
        mo.write("location", m.location);
    }
}

void write_entry_point(babel::json::object_writer& o, sgl::described_entry_point const& e)
{
    o.write("name", cc::string_view(e.name));
    o.write("stage", stage_name(e.stage));
    {
        auto grid = o.write_array("workgroup", babel::json::layout::compact);
        for (auto const n : e.workgroup)
            grid.write(n);
    }
    auto list = o.write_array("bindings", babel::json::layout::compact);
    for (auto const& name : e.bindings)
        list.write(cc::string_view(name));
}

cc::result<cc::string> to_json(sgl::module_description const& d)
{
    auto w = babel::json::string_writer({.indent = 2});
    {
        auto root = w.object();
        {
            auto bindings = root.write_array("bindings");
            for (auto const& b : d.bindings)
            {
                auto o = bindings.write_object();
                write_binding(o, b);
            }
        }
        {
            auto structs = root.write_array("structs");
            for (auto const& s : d.structs)
            {
                auto o = structs.write_object();
                write_struct(o, s);
            }
        }
        auto entries = root.write_array("entry_points");
        for (auto const& e : d.entry_points)
        {
            auto o = entries.write_object();
            write_entry_point(o, e);
        }
    }
    return w.finish();
}
} // namespace

// `sgl describe`: what the host side of one source is generated from, as JSON.
// The format is private to shaped-core's shader package generator, and nothing else should read it yet.
COMMAND("describe")
{
    auto path = cc::string();
    auto out_path = cc::string();
    auto args
        = nx::args({.name = "sgl describe",
                    .description = "Describes the bindings, the vertex and pixel structs and the entry points "
                                   "of an SGL file as JSON, or prints the diagnostics that keep it from compiling."});
    args.positional("FILE", path, {.desc = "the SGL source"});
    args.arg({"out"}, out_path, {.desc = "write the JSON here instead of to stdout", .metavar = "PATH"});
    if (auto const r = args.parse(nx::test_args()); r.should_exit())
        return r.exit_code();

    auto const source = sgl_tool::read_file(path);
    if (source.has_error())
    {
        cc::eprintln("sgl describe: cannot read {}: {}", path, source.error());
        return exit_usage;
    }

    auto const described = sgl::describe({.source = source.value(), .source_name = path});
    if (described.has_error())
    {
        cc::eprint(described.error());
        cc::flush();
        return exit_errors;
    }

    auto const json = to_json(described.value());
    if (json.has_error())
    {
        cc::eprintln("sgl describe: cannot write the description: {}", json.error().to_string());
        return exit_usage;
    }
    if (out_path.empty())
    {
        cc::print(json.value());
        cc::flush();
        return exit_ok;
    }
    if (auto const written = sgl_tool::write_file(out_path, json.value()); written.has_error())
    {
        cc::eprintln("sgl describe: cannot write {}: {}", out_path, written.error());
        return exit_usage;
    }
    return exit_ok;
}
