// lod-probe: which mip level a sampler picks, on every GPU of this machine, through dx12 and vulkan alike.
//
//   lod-probe                  probe every hardware GPU on every backend, print the tables, write lod-probe-<gpu>.json
//   lod-probe --list           the GPUs and which backends reach each, and nothing else
//   lod-probe --adapter 1      only the GPU of that index in --list
//   lod-probe --software       count WARP and lavapipe as GPUs too
//   lod-probe --images         also write the floor images that show the gap (see floor.hh)
//   lod-probe --out <prefix>   where the files go, as a path prefix (default `lod-probe`)

#include "floor.hh"
#include "probe.hh"
#include "report.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <lod_probe_shaders.hh>
#include <nexus/args/ambient.hh>
#include <nexus/async-test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-shader-library/compiler/available_compilers.hh>
#include <shaped-shader-library/shader_library.hh>

#if LOD_PROBE_HAS_DX12
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#endif
#if LOD_PROBE_HAS_VULKAN
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#endif

using namespace cc::primitive_defines;

namespace
{
/// One GPU, and the backends that reach it.
struct gpu
{
    sg::adapter_info info;
    bool on_dx12 = false;
    bool on_vulkan = false;
};

struct options
{
    bool list = false;
    bool software = false;
    bool images = false;
    int adapter = -1;
    cc::string out = "lod-probe";
};

[[nodiscard]] cc::result<options> parse(cc::span<cc::string_view const> args)
{
    auto o = options();
    for (auto i = 0; i < int(args.size()); ++i)
    {
        auto const a = args[i];
        if (a == "--list")
            o.list = true;
        else if (a == "--software")
            o.software = true;
        else if (a == "--images")
            o.images = true;
        else if ((a == "--adapter" || a == "--out") && i + 1 < int(args.size()))
        {
            auto const value = args[++i];
            if (a == "--out")
                o.out = cc::string(value);
            else
            {
                auto index = 0;
                for (auto const c : value)
                {
                    if (c < '0' || c > '9')
                        return cc::error(cc::format("--adapter takes an index, not '{}'", value));
                    index = index * 10 + (c - '0');
                }
                o.adapter = index;
            }
        }
        else
            return cc::error(cc::format("unknown argument '{}'; see the top of lod_probe.cc", a));
    }
    return o;
}

/// The same GPU on both APIs is one entry, matched by LUID; without one, by name.
void add_gpu(cc::vector<gpu>& gpus, sg::adapter_info const& info, bool dx12)
{
    for (auto& g : gpus)
    {
        auto const same
            = g.info.luid.has_value() && info.luid.has_value() ? g.info.luid == info.luid : g.info.name == info.name;
        if (same)
        {
            (dx12 ? g.on_dx12 : g.on_vulkan) = true;
            return;
        }
    }
    gpus.push_back({.info = info, .on_dx12 = dx12, .on_vulkan = !dx12});
}

[[nodiscard]] cc::vector<gpu> find_gpus(bool software)
{
    auto gpus = cc::vector<gpu>();
#if LOD_PROBE_HAS_DX12
    for (auto const& a : sg::enumerate_dx12_adapters())
        if (software || !a.is_software)
            add_gpu(gpus, a, true);
#endif
#if LOD_PROBE_HAS_VULKAN
    for (auto const& a : sg::enumerate_vulkan_adapters())
        if (software || !a.is_software)
            add_gpu(gpus, a, false);
#endif
    return gpus;
}

[[nodiscard]] cc::string vendor_name(u32 id)
{
    switch (id)
    {
    case 0x10de:
        return "NVIDIA";
    case 0x1002:
        return "AMD";
    case 0x8086:
        return "Intel";
    case 0x1414:
        return "Microsoft";
    case 0x5143:
        return "Qualcomm";
    default:
        return cc::format("{:04x}", id);
    }
}

/// A file-name-safe version of a GPU's name.
[[nodiscard]] cc::string slug(cc::string_view name)
{
    auto out = cc::string();
    for (auto const c : name)
    {
        auto const keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (keep)
            out += char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        else if (!out.empty() && out[out.size() - 1] != '-')
            out += '-';
    }
    return out;
}

[[nodiscard]] cc::result<cc::unit> write_text(cc::string_view path, cc::string_view text)
{
    auto adapter = cc::file_write_stream_adapter::create(path);
    CC_RETURN_IF_ERROR(adapter);
    cc::write_stream out = adapter.value();
    CC_RETURN_IF_ERROR(out.write(cc::as_bytes(cc::span<char const>(text.data(), text.size()))));
    CC_RETURN_IF_ERROR(out.flush());
    return cc::unit{};
}

/// One context the probe runs on: a backend, and for vulkan whether sg corrects its anisotropic LOD.
struct run_kind
{
    sg::backend_kind backend;
    bool corrected;
    cc::string_view name;
};
constexpr run_kind run_kinds[] = {
    {sg::backend_kind::dx12, true, "dx12"},
    {sg::backend_kind::vulkan, false, "vulkan"},
    {sg::backend_kind::vulkan, true, "vulkan+sg"},
};

/// A context on `g` through one backend, or the reason there is none.
[[nodiscard]] cc::result<sg::context_handle> create_on(gpu const& g, run_kind const& kind)
{
    auto const backend = kind.backend;
#if LOD_PROBE_HAS_DX12
    if (backend == sg::backend_kind::dx12)
        return sg::create_dx12_context(
            {.adapter = sg::backend::dx12::dx12_adapter::hardware_or_warp, .adapter_luid = g.info.luid});
#endif
#if LOD_PROBE_HAS_VULKAN
    if (backend == sg::backend_kind::vulkan)
        return sg::create_vulkan_context({.adapter_luid = g.info.luid, .correct_anisotropic_lod = kind.corrected});
#endif
    (void)g;
    return cc::error("this build has no such backend");
}
} // namespace

ASYNC_COMMAND("probe", default_entry)
{
    auto const parsed = parse(nx::test_args());
    if (parsed.has_error())
    {
        cc::eprintln("{}", parsed.error().to_string());
        co_return 2;
    }
    auto const& opts = parsed.value();

    auto const gpus = find_gpus(opts.software);
    if (gpus.empty())
    {
        cc::eprintln("no GPU found on dx12 or vulkan{}", opts.software ? "" : "; --software counts WARP and lavapipe");
        co_return 1;
    }
    for (auto i = 0; i < int(gpus.size()); ++i)
    {
        auto const& g = gpus[i];
        cc::println("[{}] {} ({}){}{}", i, g.info.name, vendor_name(g.info.vendor_id), g.on_dx12 ? "  dx12" : "",
                    g.on_vulkan ? "  vulkan" : "");
    }
    cc::println("");
    if (opts.list)
        co_return 0;

    // The library every context's pipelines come from; DXC compiles the SGL package once per backend format.
    auto lib = slib::shader_library();
    slib::add_available_compilers(lib);
    lib.add_package(shaders::package());

    auto const tiles = lod_probe::probe_tiles();
    auto status = 0;
    for (auto i = 0; i < int(gpus.size()); ++i)
    {
        if (opts.adapter >= 0 && opts.adapter != i)
            continue;
        auto const& g = gpus[i];
        auto results = cc::vector<lod_probe::context_result>();
        auto floors = cc::vector<lod_probe::floor_capture>();

        for (auto const& kind : run_kinds)
        {
            auto const backend = kind.backend;
            if ((backend == sg::backend_kind::dx12 && !g.on_dx12)
                || (backend == sg::backend_kind::vulkan && !g.on_vulkan))
                continue;
            auto created = create_on(g, kind);
            if (created.has_error())
            {
                cc::eprintln("{} on {}: no context: {}", g.info.name, kind.name, created.error().to_string());
                status = 1;
                continue;
            }
            auto const ctx = created.value();

            auto const measuring = lod_probe::measure(ctx);
            co_await cc::async_settled(measuring);
            auto const* const measured = measuring->try_value();
            if (measured == nullptr || measured->has_error())
            {
                cc::eprintln("{} on {}: {}", g.info.name, kind.name,
                             measured != nullptr ? measured->error().to_string() : cc::string("the probe never ran"));
                status = 1;
            }
            else
            {
                results.push_back(measured->value());
                results.back().backend = cc::string(kind.name);
            }

            if (opts.images)
            {
                auto const capturing = lod_probe::capture_floor(ctx);
                co_await cc::async_settled(capturing);
                auto const* const captured = capturing->try_value();
                if (captured == nullptr || captured->has_error())
                {
                    cc::eprintln(
                        "{} on {}: floor: {}", g.info.name, kind.name,
                        captured != nullptr ? captured->error().to_string() : cc::string("the capture never ran"));
                    status = 1;
                }
                else
                {
                    floors.push_back(captured->value());
                    floors.back().backend = cc::string(kind.name);
                }
            }

            ctx->advance_epoch();
            co_await ctx->idle_completion();
            ctx->shutdown();
        }

        if (results.empty())
            continue;
        cc::println("{}", lod_probe::text_report(results, tiles));

        auto const base = cc::format("{}-{}", opts.out, slug(g.info.name));
        if (auto const w = write_text(cc::format("{}.json", base), lod_probe::json_report(results, tiles)); w.has_error())
        {
            cc::eprintln("could not write {}.json: {}", base, w.error().to_string());
            status = 1;
        }
        else
            cc::println("wrote {}.json", base);
        if (auto const w = write_text(cc::format("{}.txt", base), lod_probe::text_report(results, tiles)); w.has_error())
            status = 1;
        else
            cc::println("wrote {}.txt", base);

        if (!floors.empty())
        {
            auto const written = lod_probe::write_floor_images(floors, base);
            if (written.has_error())
            {
                cc::eprintln("floor images: {}", written.error().to_string());
                status = 1;
            }
            else
                for (auto const& path : written.value())
                    cc::println("wrote {}", path);
        }
        cc::println("");
    }
    co_return status;
}
