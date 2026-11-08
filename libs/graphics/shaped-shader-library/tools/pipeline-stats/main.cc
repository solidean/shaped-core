#include <clean-core/algorithm/sort.hh>
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/async.hh>
#include <nexus/args/ambient.hh>
#include <nexus/args/args.hh>
#include <nexus/run.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/binding/pipeline_layout.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/exceptions.hh>
#include <shaped-graphics/query/driver_statistic.hh>
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-shader-library/compiler/available_compilers.hh>
#include <shaped-shader-library/compiler/shader_compiler.hh>
#include <shaped-shader-library/filesystem/real_filesystem.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>

#include <memory>

// `pipeline-stats compute`: what the vulkan driver says it made of an SGL compute entry point, on SGL's own SPIR-V and
// on DXC's of SGL's text, a line per figure.
// Only a driver's own compiler sees the code a GPU runs; a module's operations are what it was handed.

using namespace cc::primitive_defines;

namespace
{
// 0 = printed; 1 = bad usage, or a file, a compile or a pipeline that failed
constexpr int exit_ok = 0;
constexpr int exit_failed = 1;

[[nodiscard]] cc::optional<cc::string> read_text(cc::string_view path)
{
    auto file = cc::file_read_stream_adapter::open(path);
    if (file.has_error())
        return {};
    auto stream = file.value().stream();
    auto const bytes = stream.read_all();
    if (bytes.has_error())
        return {};
    return cc::optional<cc::string>(
        cc::string(cc::string_view(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size())));
}

/// A pipeline layout and the group layouts it is made of, by slot.
struct layout
{
    sg::pipeline_layout_handle pipeline;
    cc::vector<sg::binding_group_layout_handle> groups;
};

/// The layout a shader's own bindings state, one group per descriptor set, and an inline block where it states one.
/// SGL states an `@inline` block as the one constants buffer of no set.
[[nodiscard]] layout layout_of(sg::context& ctx, sg::compiled_shader const& shader)
{
    auto sets = cc::vector<cc::vector<sg::binding>>();
    auto desc = sg::pipeline_layout_description();
    for (auto const& b : shader.bindings)
    {
        if (!b.group_index.has_value() && b.type == sg::binding_type::constants_buffer)
        {
            desc.inline_constants = b;
            continue;
        }
        auto const set = isize(b.group_index.value_or(0));
        while (sets.size() <= set)
            sets.push_back({});
        sets[set].push_back(b);
    }
    auto result = layout();
    for (auto const& set : sets)
    {
        result.groups.push_back(ctx.cached.acquire_binding_group_layout(set));
        desc.groups.push_back(result.groups.back());
    }
    result.pipeline = ctx.cached.acquire_pipeline_layout(desc);
    return result;
}

/// One route's shader, its layout and its pipeline.
struct built
{
    sg::compiled_shader shader;
    layout layouts;
    sg::compute_pipeline_handle pipeline;
};

/// The entry point compiled on one route and its pipeline built, or why not.
[[nodiscard]] cc::result<built> build(sg::context& ctx,
                                      slib::shader_library const& lib,
                                      cc::string_view source,
                                      cc::string_view label,
                                      cc::string_view entry,
                                      bool native)
{
    auto options = cc::vector<slib::shader_option>();
    // the context prefers DXC's, so the native route is asked for by name
    slib::add_context_options(options, ctx);
    if (native)
        options.push_back({.name = cc::string(slib::k_native_spirv_option), .value = cc::string("true")});
    auto const compiled
        = lib.compile_source(source, sg::shader_stage::compute, entry, sg::shader_format::spirv,
                             {.language = slib::shader_language::sgl, .label = label, .options = options});
    (void)cc::try_async_blocking_get(compiled);
    if (compiled->has_error())
        return cc::error(compiled->try_error()->underlying().to_string());
    auto result = built{.shader = *compiled->try_value()};
    result.layouts = layout_of(ctx, result.shader);
    auto const pipeline
        = ctx.cached.acquire_compute_pipeline({.shader = result.shader, .layout = result.layouts.pipeline});
    (void)cc::try_async_blocking_get(pipeline);
    if (pipeline->has_error())
        return cc::error(pipeline->try_error()->underlying().to_string());
    result.pipeline = *pipeline->try_value();
    return result;
}

/// What the driver says of one route's pipeline: its figures, and the forms of its code where the context keeps them.
struct report
{
    cc::vector<sg::driver_statistic> statistics;
    cc::vector<sg::driver_representation> representations;
};

/// One route's pipeline and the driver's report of it, or why there is none.
[[nodiscard]] cc::result<report> figures_of(sg::context& ctx,
                                            slib::shader_library const& lib,
                                            cc::string_view source,
                                            cc::string_view label,
                                            cc::string_view entry,
                                            bool native)
{
    auto b = build(ctx, lib, source, label, entry, native);
    if (b.has_error())
        return cc::error(b.error().to_string());
    return report{.statistics = b.value().pipeline->driver_statistics(),
                  .representations = b.value().pipeline->driver_representations()};
}
/// The same, a layout or a pipeline that throws as its failure.
[[nodiscard]] cc::result<report> guarded(sg::context& ctx,
                                         slib::shader_library const& lib,
                                         cc::string_view source,
                                         cc::string_view label,
                                         cc::string_view entry,
                                         bool native)
{
    try
    {
        return figures_of(ctx, lib, source, label, entry, native);
    }
    catch (sg::exception const& e)
    {
        return cc::error(cc::string(e.message()));
    }
}
/// Writes each form of `r`'s code to `dir`, one file each: `<entry>.<route>.<executable>.<form>.txt`.
void write_forms(cc::string_view dir, cc::string_view entry, cc::string_view route, report const& r)
{
    // a name as a file's: letters, digits and dashes
    auto const file_name = [](cc::string_view name)
    {
        auto out = cc::string();
        for (auto const ch : name)
            out.push_back((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ? ch : '-');
        return out;
    };
    for (auto const& f : r.representations)
    {
        if (f.text.empty())
            continue;
        auto const path = cc::format("{}/{}.{}.{}.{}.txt", dir, entry, route, file_name(f.executable), file_name(f.name));
        auto file = cc::file_write_stream_adapter::create(path);
        if (file.has_error())
        {
            CC_LOG_ERROR("cannot write {}: {}", path, file.error().to_string());
            continue;
        }
        auto stream = file.value().stream();
        (void)stream.write(cc::span<byte const>(reinterpret_cast<byte const*>(f.text.data()), f.text.size()));
        (void)stream.flush();
    }
}
} // namespace

COMMAND("compute")
{
    auto path = cc::string();
    auto entries = cc::vector<cc::string>();
    auto module_dirs = cc::vector<cc::string>();
    auto forms_dir = cc::string();
    auto args
        = nx::args({.name = "pipeline-stats compute",
                    .description = "Prints what the vulkan driver reports of each compute entry point's pipeline, "
                                   "on SGL's own SPIR-V and on DXC's of SGL's text: a line per figure, "
                                   "`entry executable name native dxc`, tab-separated."});
    args.positional("FILE", path, {.desc = "the .sgl source"});
    args.arg({"entry"}, entries, {.desc = "a compute entry point; any number", .metavar = "NAME"});
    args.arg({"module-dir"}, module_dirs,
             {.desc = "a directory of SGL modules the source may use, absolute", .metavar = "DIR"});
    args.arg({"isa"}, forms_dir,
             {.desc = "a directory to write the driver's forms of each pipeline's code to, its ISA among them",
              .metavar = "DIR"});
    if (auto const r = args.parse(nx::test_args()); r.should_exit())
        return r.exit_code();
    auto const source = read_text(path);
    if (!source.has_value() || entries.empty())
    {
        CC_LOG_ERROR("name a readable .sgl file and at least one entry point");
        return exit_failed;
    }

    auto created = sg::create_vulkan_context(
        {.native_spirv = false, .capture_driver_statistics = true, .capture_driver_representations = !forms_dir.empty()});
    if (created.has_error())
    {
        CC_LOG_ERROR("no vulkan context: {}", created.error().to_string());
        return exit_failed;
    }
    auto& ctx = *created.value();
    auto lib = slib::shader_library();
    slib::add_available_compilers(lib);
    for (auto i = isize(0); i < module_dirs.size(); ++i)
    {
        auto const mount = cc::format("modules{}", i);
        lib.mount(mount, std::make_shared<slib::real_filesystem>(module_dirs[i]));
        lib.add_module_dir(mount);
    }

    auto failed = false;
    for (auto const& entry : entries)
    {
        auto const native = guarded(ctx, lib, source.value(), path, entry, true);
        auto const dxc = guarded(ctx, lib, source.value(), path, entry, false);
        if (native.has_error() || dxc.has_error())
        {
            CC_LOG_ERROR("{}: {}", entry, native.has_error() ? native.error().to_string() : dxc.error().to_string());
            failed = true;
            continue;
        }
        if (!forms_dir.empty())
        {
            write_forms(forms_dir, entry, "native", native.value());
            write_forms(forms_dir, entry, "dxc", dxc.value());
        }
        for (auto const& n : native.value().statistics)
        {
            auto other = cc::string("-");
            for (auto const& d : dxc.value().statistics)
                if (d.executable == n.executable && d.name == n.name)
                    other = cc::format("{}", d.value);
            cc::println("{}\t{}\t{}\t{}\t{}", entry, n.executable, n.name, n.value, other);
        }
    }
    return failed ? exit_failed : exit_ok;
}

namespace
{
constexpr auto k_chains = 4;
constexpr auto k_trips = 256;

/// A kernel of four independent chains, each `body` 256 times, between a read and a write of every thread's values.
/// `@` in `body` is each chain's float and `#` its uint; `s` and `t` are a float and a uint of the thread's, set before
/// the loop; a body of several steps puts one on each line.
/// A recurrence by a thread's own value is none a driver solves in closed form, which it does for one by constants.
/// The chains read nothing of each other, so a GPU may run all four at once and the kernel measures throughput.
/// An empty body is the kernel's own cost alone.
[[nodiscard]] cc::string kernel_of(cc::string_view body)
{
    auto source = cc::string("binding work:\n"
                             "    values: mut buffer[float]\n"
                             "    bits: mut buffer[uint]\n"
                             "\n"
                             "@compute(64) fun run(@thread_id id: int3){work}:\n"
                             "    let i = id.x\n"
                             "    let mut a = work.values[i]\n"
                             "    let mut b = a + 1.0\n"
                             "    let mut c = a + 2.0\n"
                             "    let mut d = a + 3.0\n"
                             "    let mut x = work.bits[i]\n"
                             "    let mut y = x + 1u\n"
                             "    let mut z = x + 2u\n"
                             "    let mut w = x + 3u\n"
                             "    let s = a * 0.5 + 0.25\n"
                             "    let t = x | 1u\n");
    if (!body.empty())
    {
        source.appendf("    for k in 0 ..< {}:\n", k_trips);
        constexpr cc::string_view floats = "abcd";
        constexpr cc::string_view uints = "xyzw";
        for (auto chain = isize(0); chain < k_chains; ++chain)
        {
            source.append("        ");
            for (auto const ch : body)
            {
                source.push_back(ch == '@' ? floats[chain] : ch == '#' ? uints[chain] : ch);
                if (ch == '\n')
                    source.append("        ");
            }
            source.append("\n");
        }
    }
    source.append("    work.values[i] = a + b + c + d\n"
                  "    work.bits[i] = x + y + z + w\n");
    return source;
}

/// A kernel that measures one operation: its body, and the full-rate operations each step spends beside it.
/// A step is no affine function of the chain it updates, which a driver would solve for all its trips at once.
struct kernel
{
    cc::string_view name;
    cc::string_view body;
    f64 alu_beside = 0;
    /// The operations it measures each step.
    f64 count = 1;
};
} // namespace

// `pipeline-stats throughput`: what each operation costs this GPU, relative to an fma, from kernels that spend in one each.
COMMAND("throughput")
{
    auto threads = 1 << 20;
    auto args = nx::args({.name = "pipeline-stats throughput",
                          .description = "Times a generated kernel per operation on the vulkan GPU that SC_GPU "
                                         "picks, and prints each operation's cost relative to an fma: its time past "
                                         "an empty kernel's, less the full-rate operations beside it, per operation."});
    if (auto const r = args.parse(nx::test_args()); r.should_exit())
        return r.exit_code();
    auto created = sg::create_vulkan_context({.native_spirv = false});
    if (created.has_error())
    {
        CC_LOG_ERROR("no vulkan context: {}", created.error().to_string());
        return exit_failed;
    }
    auto& ctx = *created.value();
    if (!ctx.supports(sg::feature::timestamp_query))
    {
        CC_LOG_ERROR("this device has no timestamp queries");
        return exit_failed;
    }
    auto lib = slib::shader_library();
    slib::add_available_compilers(lib);
    auto const usage = sg::buffer_usage::readwrite_buffer;
    auto const values = ctx.persistent.create_buffer_from_data(cc::vector<f32>::create_filled(threads, 0.75f), usage);
    auto const bits = ctx.persistent.create_buffer_from_data(cc::vector<u32>::create_filled(threads, 12345u), usage);

    // the first two are what every other is measured against; see kernel_of for `@`, `#`, `s` and `t`
    auto const kernels = cc::vector<kernel>{
        {.name = "empty", .body = ""},
        {.name = "fma", .body = "@ = fma(@, @, s)"},
        {.name = "mul_add", .body = "@ = @ * @ + s", .alu_beside = 1},
        {.name = "sqrt", .body = "@ = sqrt(@ + s)", .alu_beside = 1},
        {.name = "exp2", .body = "@ = exp2(@ * s)", .alu_beside = 1},
        {.name = "sin", .body = "@ = sin(@ + s)", .alu_beside = 1},
        {.name = "divide", .body = "@ = s / @ + 0.5", .alu_beside = 1},
        {.name = "int_multiply", .body = "# = # * # + t", .alu_beside = 1},
        {.name = "int_divide", .body = "# = # / t + 7u", .alu_beside = 1},
        {.name = "conversion", .body = "@ = (# as float) * @\n# = (@ as uint) + t", .alu_beside = 1, .count = 2},
    };
    auto per_thread = cc::vector<f64>();
    for (auto const& k : kernels)
    {
        auto b = build(ctx, lib, kernel_of(k.body), k.name, "run", true);
        if (b.has_error())
        {
            CC_LOG_ERROR("{}: {}", k.name, b.error().to_string());
            return exit_failed;
        }
        constexpr auto k_rounds = 9;
        constexpr auto k_batch = 4;
        auto spans = cc::vector<cc::pair<sg::gpu_timestamp, sg::gpu_timestamp>>();
        for (auto r = 0; r < k_rounds; ++r)
        {
            auto cmd = ctx.create_command_list();
            auto const group = ctx.transient.create_binding_group(
                b.value().layouts.groups[0],
                cc::vector<sg::named_view>{{.name = "work.values", .view = values.as_readwrite_buffer()},
                                           {.name = "work.bits", .view = bits.as_readwrite_buffer()}});
            auto const from = cmd->query.record_gpu_timestamp();
            for (auto j = 0; j < k_batch; ++j)
            {
                cmd->compute.bind_pipeline(*b.value().pipeline);
                cmd->compute.bind_group(0, *group);
                cmd->compute.dispatch_threads(u32(threads));
            }
            auto const to = cmd->query.record_gpu_timestamp();
            ctx.submit_command_list(cc::move(cmd));
            ctx.advance_epoch();
            spans.push_back({from, to});
        }
        (void)cc::try_async_blocking_get(ctx.idle_completion());
        auto ns = cc::vector<f64>();
        for (auto const& [from, to] : spans)
            ns.push_back(1e9 * (to.try_get_seconds().value() - from.try_get_seconds().value()) / k_batch);
        cc::sort(ns);
        // the median, which the rounds a driver's tiers warm in do not reach
        per_thread.push_back(ns[ns.size() / 2] / f64(threads));
    }

    auto const operations = f64(k_chains * k_trips);
    auto const fma = (per_thread[1] - per_thread[0]) / operations;
    cc::println("operation\tns per thread\tcost of an fma");
    for (auto i = isize(0); i < kernels.size(); ++i)
    {
        auto const own = per_thread[i] - per_thread[0] - kernels[i].alu_beside * fma * operations;
        cc::println("{}\t{:.4f}\t{:.2f}", kernels[i].name, per_thread[i],
                    i == 0 ? 0.0 : own / (operations * kernels[i].count) / fma);
    }
    return exit_ok;
}

int main(int argc, char** argv)
{
    return nx::run(argc, argv);
}
