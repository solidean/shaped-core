#include "files.hh"
#include "module_dirs.hh"

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

/// The stage as the package's entry lists name it, and `none` for no stage.
cc::string_view stage_word(sgl::check::stage s)
{
    return s == sgl::check::stage::none ? cc::string_view("none") : sgl::check::stage_name(s);
}

cc::string_view kind_name(sgl::described_member_kind k)
{
    switch (k)
    {
    case sgl::described_member_kind::constant:
        return "constant";
    case sgl::described_member_kind::buffer:
        return "buffer";
    case sgl::described_member_kind::bytes:
        return "bytes";
    case sgl::described_member_kind::texture:
        return "texture";
    case sgl::described_member_kind::image:
        return "image";
    case sgl::described_member_kind::sampler:
        return "sampler";
    case sgl::described_member_kind::acceleration_structure:
        return "acceleration_structure";
    case sgl::described_member_kind::constants_buffer:
        return "constants";
    }
    return "constant";
}

void write_settings(babel::json::object_writer& so, sgl::described_sampler const& st)
{
    so.write("min_filter", cc::string_view(st.min_filter));
    so.write("mag_filter", cc::string_view(st.mag_filter));
    so.write("mip_filter", cc::string_view(st.mip_filter));
    so.write("address_u", cc::string_view(st.address_u));
    so.write("address_v", cc::string_view(st.address_v));
    so.write("address_w", cc::string_view(st.address_w));
    if (!st.compare.empty())
        so.write("compare", cc::string_view(st.compare));
    so.write("max_anisotropy", st.max_anisotropy);
    so.write("min_lod", st.min_lod);
    so.write("max_lod", st.max_lod);
    so.write("mip_lod_bias", st.mip_lod_bias);
}

void write_binding(babel::json::object_writer& o, sgl::described_binding const& b)
{
    o.write("name", cc::string_view(b.name));
    o.write("inline", b.is_inline);
    o.write("shape", cc::string_view(b.shape));
    o.write("block_size", b.block_size);
    {
        auto list = o.write_array("options", babel::json::layout::compact);
        for (auto const& name : b.options)
            list.write(cc::string_view(name));
    }
    if (b.block_slot >= 0)
        o.write("block_slot", b.block_slot);
    // an `@inline` binding's block takes no slot and is named all the same, which slib's inline binding names it by
    if (!b.block_host_name.empty())
        o.write("block_host_name", cc::string_view(b.block_host_name));
    auto members = o.write_array("members");
    for (auto const& m : b.members)
    {
        auto mo = members.write_object(babel::json::layout::compact);
        mo.write("name", cc::string_view(m.name));
        mo.write("kind", kind_name(m.kind));
        mo.write("type", cc::string_view(m.type));
        if (m.kind == sgl::described_member_kind::constant)
        {
            mo.write("offset", m.offset);
            mo.write("size", m.size);
            continue;
        }
        mo.write("access", cc::string_view(m.access));
        mo.write("slot", m.slot);
        if (m.kind == sgl::described_member_kind::buffer)
            mo.write("stride", m.stride);
        // the bytes a constant buffer's view must hold, which the host's `T` is
        if (m.kind == sgl::described_member_kind::constants_buffer)
            mo.write("size", m.size);
        // a binding array's length, 0 where unbounded (CHK-490), and 1 for any other resource
        if (m.count != 1)
            mo.write("count", m.count);
        mo.write("host_name", cc::string_view(m.host_name));
        // The sg enum values a binding of this kind states, each written only where it applies.
        auto const optional = [&](cc::string_view key, cc::string const& value)
        {
            if (!value.empty())
                mo.write(key, cc::string_view(value));
        };
        optional("texture_dimension", m.texture_dimension);
        optional("sample_type", m.sample_type);
        optional("image_format", m.image_format);
        optional("format_option", m.format_option);
        optional("count_option", m.count_option);
        optional("sampler_type", m.sampler_type);
        if (m.static_sampler.has_value())
        {
            auto so = mo.write_object("static_sampler", babel::json::layout::compact);
            write_settings(so, m.static_sampler.value());
        }
    }
}

void write_struct(babel::json::object_writer& o, sgl::described_struct const& s)
{
    o.write("name", cc::string_view(s.name));
    o.write("edge", stage_word(s.edge));
    o.write("shape", cc::string_view(s.shape));
    auto members = o.write_array("members");
    for (auto const& m : s.members)
    {
        auto mo = members.write_object(babel::json::layout::compact);
        mo.write("name", cc::string_view(m.name));
        mo.write("type", cc::string_view(m.type));
        mo.write("location", m.location);
        if (!m.stream.empty())
        {
            mo.write("stream", cc::string_view(m.stream));
            mo.write("per_instance", m.is_per_instance);
        }
        if (!m.format.empty())
            mo.write("format", cc::string_view(m.format));
        if (!m.output.empty())
            mo.write("output", cc::string_view(m.output));
        if (!m.semantic.empty())
            mo.write("semantic", cc::string_view(m.semantic));
        if (m.pieces > 0)
        {
            mo.write("pieces", m.pieces);
            mo.write("piece", cc::string_view(m.piece));
        }
    }
}

void write_memory_struct(babel::json::object_writer& o, sgl::described_memory_struct const& s)
{
    o.write("name", cc::string_view(s.name));
    if (!s.module.empty())
        o.write("module", cc::string_view(s.module));
    o.write("space", cc::string_view(s.space));
    o.write("size", s.size);
    {
        auto members = o.write_array("members");
        for (auto const& m : s.members)
        {
            auto mo = members.write_object(babel::json::layout::compact);
            mo.write("name", cc::string_view(m.name));
            mo.write("type", cc::string_view(m.type));
            mo.write("offset", m.offset);
            mo.write("size", m.size);
        }
    }
    if (s.cases.empty())
        return;
    {
        auto cases = o.write_array("cases");
        for (auto const& c : s.cases)
        {
            auto co = cases.write_object(babel::json::layout::compact);
            co.write("name", cc::string_view(c.name));
            if (!c.parent.empty())
                co.write("parent", cc::string_view(c.parent));
            co.write("leaf", c.leaf);
            co.write("condition", cc::string_view(c.condition));
            co.write("buildable", c.is_buildable);
            auto writes = co.write_object("writes");
            for (auto const& [field, value] : c.writes)
                writes.write(field, cc::string_view(value));
        }
    }
    auto variants = o.write_array("variant_fields");
    for (auto const& v : s.variant_fields)
    {
        auto vo = variants.write_object(babel::json::layout::compact);
        vo.write("name", cc::string_view(v.name));
        vo.write("type", cc::string_view(v.type));
        vo.write("offset", v.offset);
        vo.write("size", v.size);
        {
            auto holders = vo.write_array("cases");
            for (auto const& c : v.cases)
                holders.write(cc::string_view(c));
        }
        if (!v.declared_in.empty())
            vo.write("declared_in", cc::string_view(v.declared_in));
        vo.write("discriminant", v.is_discriminant);
    }
}

void write_entry_point(babel::json::object_writer& o, sgl::described_entry_point const& e)
{
    o.write("name", cc::string_view(e.name));
    o.write("stage", stage_word(e.stage));
    {
        auto grid = o.write_array("workgroup", babel::json::layout::compact);
        for (auto const n : e.workgroup)
            grid.write(n);
    }
    // CHK-371: only an entry point that asks for a size says so
    if (e.preferred_subgroup_size > 0)
        o.write("preferred_subgroup_size", e.preferred_subgroup_size);
    // CHK-435: what a host would weaken on WebGPU, and the function to read before it does
    if (e.float_guarantee != sgl::check::float_level::fast)
    {
        cc::string_view const names[] = {"fast", "specials", "precise", "ieee"};
        o.write("float_guarantee", names[int(e.float_guarantee)]);
        o.write("float_raised_by", e.float_raised_by);
        if (e.is_float_raised_by_floor)
            o.write("float_raised_by_floor", true);
    }
    {
        auto list = o.write_array("bindings", babel::json::layout::compact);
        for (auto const& name : e.bindings)
            list.write(cc::string_view(name));
    }
    {
        auto list = o.write_array("features", babel::json::layout::compact);
        for (auto const& name : e.features)
            list.write(cc::string_view(name));
    }
    {
        auto list = o.write_array("samplers", babel::json::layout::compact);
        for (auto const& name : e.samplers)
            list.write(cc::string_view(name));
    }
    {
        auto list = o.write_array("options", babel::json::layout::compact);
        for (auto const& name : e.options)
            list.write(cc::string_view(name));
    }
    // EMIT-268 to EMIT-270: what the signature hands a pipeline, which a host that builds its own pipelines reads
    o.write("vertex_input", cc::string_view(e.vertex_input));
    {
        auto list = o.write_array("vertex_attributes");
        for (auto const& a : e.vertex_attributes)
        {
            auto ao = list.write_object(babel::json::layout::compact);
            ao.write("name", cc::string_view(a.name));
            ao.write("type", cc::string_view(a.type));
            ao.write("semantic", cc::string_view(a.semantic));
            ao.write("semantic_index", a.semantic_index);
            ao.write("location", a.location);
            if (!a.format.empty())
                ao.write("format", cc::string_view(a.format));
            ao.write("stream", cc::string_view(a.stream));
            ao.write("per_instance", a.is_per_instance);
        }
    }
    o.write("payload", cc::string_view(e.payload));
    o.write("payload_shape", cc::string_view(e.payload_shape));
    o.write("payload_size", e.payload_size);
    o.write("attributes", cc::string_view(e.attributes));
    o.write("attributes_shape", cc::string_view(e.attributes_shape));
    o.write("attribute_size", e.attribute_size);
    // One `slot: access` per touched slot, the way a corpus pin spells it.
    auto list = o.write_array("footprint", babel::json::layout::compact);
    for (auto const& slot : e.footprint)
        list.write(cc::string_view(sgl::check::footprint_text(cc::span<sgl::check::slot_footprint const>(&slot, 1))));
}

void write_file_sampler(babel::json::object_writer& o, sgl::described_file_sampler const& s)
{
    o.write("name", cc::string_view(s.name));
    o.write("index", s.index);
    o.write("sampler_type", cc::string_view(s.sampler_type));
    o.write("shape", cc::string_view(s.shape));
    auto so = o.write_object("settings", babel::json::layout::compact);
    write_settings(so, s.settings);
}

void write_names(babel::json::object_writer& o, cc::string_view key, cc::span<cc::string const> names)
{
    auto list = o.write_array(key, babel::json::layout::compact);
    for (auto const& name : names)
        list.write(cc::string_view(name));
}

void write_ray_set(babel::json::object_writer& o, sgl::described_ray_set const& s)
{
    o.write("name", cc::string_view(s.name));
    write_names(o, "rays", s.rays);
    write_names(o, "payloads", s.payloads);
    {
        auto list = o.write_array("payload_sizes", babel::json::layout::compact);
        for (auto const size : s.payload_sizes)
            list.write(i64(size));
    }
    write_names(o, "payload_shapes", s.payload_shapes);
}

void write_hit_group(babel::json::object_writer& o, sgl::described_hit_group const& g)
{
    o.write("name", cc::string_view(g.name));
    o.write("rays", cc::string_view(g.ray_set));
    o.write("geometry", g.is_procedural ? "procedural" : "triangles");
    o.write("intersection", cc::string_view(g.intersection));
    write_names(o, "closest_hits", g.closest_hits);
    write_names(o, "any_hits", g.any_hits);
    write_names(o, "traversals", g.traversals);
}

void write_raytracing_pipeline(babel::json::object_writer& o, sgl::described_raytracing_pipeline const& p)
{
    o.write("name", cc::string_view(p.name));
    o.write("rays", cc::string_view(p.ray_set));
    o.write("raygen", cc::string_view(p.raygen));
    write_names(o, "misses", p.misses);
    write_names(o, "hit_groups", p.hit_groups);
    o.write("host_hit_groups", p.has_host_hit_groups);
    o.write("max_recursion_depth", i64(p.max_recursion_depth));
    o.write("max_payload_size", i64(p.max_payload_size));
    o.write("max_attribute_size", i64(p.max_attribute_size));
    write_names(o, "layout", p.layout);
    o.write("inline", cc::string_view(p.inline_constants));
    write_names(o, "features", p.features);
    write_names(o, "callables", p.callables);
    o.write("host_callables", p.has_host_callables);
    o.write("host_callable_parameter", cc::string_view(p.host_callable_parameter));
    o.write("host_callable_shape", cc::string_view(p.host_callable_shape));
    write_names(o, "samplers", p.samplers);
    write_names(o, "options", p.options);
    auto frozen = o.write_array("frozen");
    for (auto const& line : p.frozen)
        frozen.write(cc::string_view(line));
}

void write_callables(babel::json::object_writer& o, sgl::described_callables const& c)
{
    o.write("name", cc::string_view(c.name));
    o.write("parameter", cc::string_view(c.parameter));
    o.write("parameter_shape", cc::string_view(c.parameter_shape));
    write_names(o, "entries", c.entries);
    o.write("host", c.has_host);
    o.write("offset", i64(c.offset));
}

void write_pipeline(babel::json::object_writer& o, sgl::described_pipeline const& p)
{
    o.write("name", cc::string_view(p.name));
    o.write("vertex", cc::string_view(p.vertex));
    o.write("pixel", cc::string_view(p.pixel));
    o.write("geometry", cc::string_view(p.geometry));
    o.write("geometry_corners", i64(p.geometry_corners));
    o.write("tessellation_control", cc::string_view(p.tessellation_control));
    o.write("tessellation_evaluation", cc::string_view(p.tessellation_evaluation));
    o.write("mesh", cc::string_view(p.mesh));
    o.write("task", cc::string_view(p.task));
    {
        auto list = o.write_array("layout", babel::json::layout::compact);
        for (auto const& name : p.layout)
            list.write(cc::string_view(name));
    }
    o.write("inline", cc::string_view(p.inline_constants));
    o.write("vertex_input", cc::string_view(p.vertex_input));
    o.write("target_set", cc::string_view(p.target_set));
    {
        auto list = o.write_array("targets", babel::json::layout::compact);
        for (auto const& name : p.targets)
            list.write(cc::string_view(name));
    }
    {
        auto list = o.write_array("features", babel::json::layout::compact);
        for (auto const& name : p.features)
            list.write(cc::string_view(name));
    }
    {
        auto list = o.write_array("samplers", babel::json::layout::compact);
        for (auto const& name : p.samplers)
            list.write(cc::string_view(name));
    }
    {
        auto settings = o.write_array("settings");
        for (auto const& s : p.settings)
        {
            auto so = settings.write_object(babel::json::layout::compact);
            so.write("path", cc::string_view(s.path));
            switch (s.kind)
            {
            case sgl::check::setting_kind::boolean:
                so.write("kind", "bool");
                so.write("value", s.integer != 0);
                break;
            case sgl::check::setting_kind::integer:
                so.write("kind", "int");
                so.write("value", s.integer);
                break;
            case sgl::check::setting_kind::real:
                so.write("kind", "float");
                so.write("value", s.real);
                break;
            case sgl::check::setting_kind::enum_case:
                so.write("kind", "case");
                so.write("value", cc::string_view(s.enum_case));
                so.write("enum", cc::string_view(s.enum_name));
                break;
            case sgl::check::setting_kind::host:
                so.write("kind", "host");
                break;
            case sgl::check::setting_kind::none:
                so.write("kind", "none");
                break;
            }
        }
    }
    {
        auto open = o.write_array("open", babel::json::layout::compact);
        for (auto const& path : p.open)
            open.write(cc::string_view(path));
    }
    {
        auto list = o.write_array("options", babel::json::layout::compact);
        for (auto const& name : p.options)
            list.write(cc::string_view(name));
    }
    auto frozen = o.write_array("frozen");
    for (auto const& line : p.frozen)
        frozen.write(cc::string_view(line));
}

cc::result<cc::string> to_json(sgl::module_description const& d)
{
    auto w = babel::json::string_writer({.indent = 2});
    {
        auto root = w.object();
        {
            auto options = root.write_array("options");
            for (auto const& option : d.options)
            {
                auto o = options.write_object(babel::json::layout::compact);
                o.write("name", cc::string_view(option.name));
                o.write("type", cc::string_view(option.type));
                o.write("value", cc::string_view(option.value));
                if (!option.follows.empty())
                {
                    auto follows = o.write_array("follows", babel::json::layout::compact);
                    for (auto const& name : option.follows)
                        follows.write(cc::string_view(name));
                }
            }
        }
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
        {
            auto structs = root.write_array("memory_structs");
            for (auto const& s : d.memory_structs)
            {
                auto o = structs.write_object();
                write_memory_struct(o, s);
            }
        }
        {
            auto entries = root.write_array("entry_points");
            for (auto const& e : d.entry_points)
            {
                auto o = entries.write_object();
                write_entry_point(o, e);
            }
        }
        {
            auto pipelines = root.write_array("pipelines");
            for (auto const& p : d.pipelines)
            {
                auto o = pipelines.write_object();
                write_pipeline(o, p);
            }
        }
        {
            auto sets = root.write_array("ray_sets");
            for (auto const& r : d.ray_sets)
            {
                auto o = sets.write_object();
                write_ray_set(o, r);
            }
        }
        {
            auto groups = root.write_array("hit_groups");
            for (auto const& g : d.hit_groups)
            {
                auto o = groups.write_object();
                write_hit_group(o, g);
            }
        }
        {
            auto pipelines = root.write_array("raytracing_pipelines");
            for (auto const& p : d.raytracing_pipelines)
            {
                auto o = pipelines.write_object();
                write_raytracing_pipeline(o, p);
            }
        }
        {
            auto tables = root.write_array("callables");
            for (auto const& c : d.callables)
            {
                auto o = tables.write_object();
                write_callables(o, c);
            }
        }
        auto samplers = root.write_array("samplers");
        for (auto const& s : d.samplers)
        {
            auto o = samplers.write_object();
            write_file_sampler(o, s);
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
    auto option_arguments = cc::vector<cc::string>();
    auto module = cc::string();
    auto module_dirs = cc::vector<cc::string>();
    auto float_floor_name = cc::string("fast");
    auto args = nx::args(
        {.name = "sgl describe",
         .description = "Describes the bindings, the vertex and pixel structs, the entry points and the "
                        "pipelines of an SGL file as JSON, or prints the diagnostics that keep it from compiling."});
    args.positional("FILE", path, {.desc = "the SGL source; none with --module"});
    args.arg({"out"}, out_path, {.desc = "write the JSON here instead of to stdout", .metavar = "PATH"});
    args.arg({"option"}, option_arguments,
             {.desc = "describe with an option of the source set, `--option tile=16`; repeat for each",
              .metavar = "NAME=VALUE"});
    args.arg({"module-dir"}, module_dirs,
             {.desc = "a directory whose .sgl files are modules the file may use", .metavar = "DIR"});
    args.arg({"module"}, module,
             {.desc = "describe this module of the module directories instead of a file", .metavar = "NAME"});
    args.arg({"float-floor"}, float_floor_name,
             {.desc = "the least float guarantee every entry point is described at, as if each stated it: fast, "
                      "specials, precise or ieee",
              .metavar = "LEVEL"});
    if (auto const r = args.parse(nx::test_args()); r.should_exit())
        return r.exit_code();
    if (path.empty() == module.empty())
    {
        cc::eprintln("sgl describe: give either a FILE or --module NAME");
        return exit_usage;
    }
    auto const library = sgl_tool::read_module_dirs(module_dirs);
    if (library.has_error())
    {
        cc::eprintln("sgl describe: {}", library.error());
        return exit_usage;
    }

    auto const options = sgl_tool::parse_options(option_arguments);
    if (options.has_error())
    {
        cc::eprintln("sgl describe: an option is given as `name=value`, and '{}' is not", options.error());
        return exit_usage;
    }
    auto float_floor = sgl::check::float_level::fast;
    if (float_floor_name == "specials")
        float_floor = sgl::check::float_level::specials;
    else if (float_floor_name == "precise")
        float_floor = sgl::check::float_level::precise;
    else if (float_floor_name == "ieee")
        float_floor = sgl::check::float_level::ieee;
    else if (float_floor_name != "fast")
    {
        cc::eprintln("sgl describe: unknown --float-floor '{}' (one of fast, specials, precise, ieee)", float_floor_name);
        return exit_usage;
    }

    auto source = cc::result<cc::string, cc::string>(cc::string());
    if (!path.empty())
        source = sgl_tool::read_file(path);
    if (source.has_error())
    {
        cc::eprintln("sgl describe: cannot read {}: {}", path, source.error());
        return exit_usage;
    }

    auto const described = sgl::describe({.source = source.value(),
                                          .source_name = path,
                                          .options = options.value(),
                                          .library = sgl_tool::files_but(library.value(), path),
                                          .module = module,
                                          .float_floor = float_floor});
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

// EMIT-268 to EMIT-270: every entry point carries its interface's keys, empty or 0 where its stage has none, so a host
// reading them needs no case for a missing one.
TEST("sgl describe - each entry point's JSON names its vertex input, its payload and its hit attributes")
{
    auto const described = sgl::describe({.source = "require raytracing_pipeline\n"
                                                    "@vertex struct mesh_vertex:\n"
                                                    "    position: pos3\n"
                                                    "struct varyings:\n"
                                                    "    @position position: hpos4\n"
                                                    "@vertex fun fed(v: mesh_vertex) -> varyings:\n"
                                                    "    return { position = hpos4(v.position.x, 0.0, 0.0, 1.0) }\n"
                                                    "struct radiance:\n"
                                                    "    color: float3\n"
                                                    "struct sphere_attributes:\n"
                                                    "    u: float\n"
                                                    "    v: float\n"
                                                    "    w: float\n"
                                                    "@closest_hit fun shade(h: procedural_hit[sphere_attributes], p: "
                                                    "mut radiance):\n"
                                                    "    p.color = float3(h.attributes.u, 0.0, 0.0)\n",
                                          .source_name = "t.sgl"});
    REQUIRE(described.has_value());
    auto const json = to_json(described.value());
    REQUIRE(json.has_value());
    auto const doc = babel::json::read(cc::string_view(json.value()));
    REQUIRE(doc.has_value());
    auto const entries = doc.value().root()["entry_points"];
    REQUIRE(entries.size() == 2);
    auto const fed = entries[0];
    CHECK(fed["name"].as_string() == "fed");
    CHECK(fed["vertex_input"].as_string() == "mesh_vertex");
    CHECK(fed["payload"].as_string() == "");
    CHECK(fed["payload_size"].as_double(-1) == 0);
    CHECK(fed["attributes"].as_string() == "");
    CHECK(fed["attributes_shape"].as_string() == "");
    CHECK(fed["attribute_size"].as_double(-1) == 0);
    auto const shade = entries[1];
    CHECK(shade["name"].as_string() == "shade");
    CHECK(shade["vertex_input"].as_string() == "");
    CHECK(shade["payload"].as_string() == "radiance");
    CHECK(shade["payload_size"].as_double() == 12);
    CHECK(shade["attributes"].as_string() == "sphere_attributes");
    CHECK(shade["attributes_shape"].as_string() == cc::string_view(described.value().entry_points[1].attributes_shape));
    CHECK(shade["attributes_shape"].as_string().size() == 32);
    CHECK(shade["attribute_size"].as_double() == 12);
}

// CHK-171: a module's `@inline` block is named by the module, which slib's generated inline binding reads from here
TEST("sgl describe - a module's @inline binding's JSON names its block by the module")
{
    sgl::library_file const library[] = {{.name = "push.sgl",
                                          .source = "module push\n"
                                                    "@inline binding constants:\n"
                                                    "    scale: float\n"}};
    auto const described = sgl::describe({.library = library, .module = "push"});
    REQUIRE(described.has_value());
    auto const json = to_json(described.value());
    REQUIRE(json.has_value());
    auto const doc = babel::json::read(cc::string_view(json.value()));
    REQUIRE(doc.has_value());
    auto const bindings = doc.value().root()["bindings"];
    REQUIRE(bindings.size() == 1);
    CHECK(bindings[0]["inline"].as_bool());
    CHECK(bindings[0]["block_host_name"].as_string() == "push.constants");
    // and it takes no slot, which slib reads as no group block
    CHECK(!bindings[0]["block_slot"].is_valid());
}

// EMIT-268: a vertex entry point's attributes, one record per attribute in location order, `format` only where `@format`
// states one
TEST("sgl describe - a vertex entry point's JSON lists its vertex attributes")
{
    auto const described
        = sgl::describe({.source = "@vertex struct mesh_vertex:\n"
                                   "    position: pos3\n"
                                   "    @per_instance @format(.rgba8_unorm) tint: float4\n"
                                   "struct varyings:\n"
                                   "    @position position: hpos4\n"
                                   "@vertex fun fed(v: mesh_vertex) -> varyings:\n"
                                   "    return { position = hpos4(v.position.x + v.tint.x, 0.0, 0.0, 1.0) }\n"
                                   "@compute(1) fun work(@thread_id id: int3):\n"
                                   "    let x = id.x\n",
                         .source_name = "t.sgl"});
    REQUIRE(described.has_value());
    auto const json = to_json(described.value());
    REQUIRE(json.has_value());
    auto const doc = babel::json::read(cc::string_view(json.value()));
    REQUIRE(doc.has_value());
    auto const entries = doc.value().root()["entry_points"];
    REQUIRE(entries.size() == 2);
    auto const attributes = entries[0]["vertex_attributes"];
    REQUIRE(attributes.size() == 2);
    CHECK(attributes[0]["name"].as_string() == "position");
    CHECK(attributes[0]["type"].as_string() == "pos3");
    CHECK(attributes[0]["semantic"].as_string() == "POSITION");
    CHECK(attributes[0]["semantic_index"].as_double(-1) == 0);
    CHECK(attributes[0]["location"].as_double(-1) == 0);
    CHECK(!attributes[0].has("format"));
    CHECK(attributes[0]["stream"].as_string() == "per_vertex");
    CHECK(!attributes[0]["per_instance"].as_bool(true));
    CHECK(attributes[1]["name"].as_string() == "tint");
    CHECK(attributes[1]["location"].as_double() == 1);
    CHECK(attributes[1]["format"].as_string() == "rgba8_unorm");
    CHECK(attributes[1]["stream"].as_string() == "per_instance");
    CHECK(attributes[1]["per_instance"].as_bool());
    // every other entry point states the key, empty
    CHECK(entries[1]["vertex_attributes"].is_array());
    CHECK(entries[1]["vertex_attributes"].size() == 0);
}
