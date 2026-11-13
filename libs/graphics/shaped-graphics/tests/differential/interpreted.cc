#include "differential.hh"

#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <nexus/source_root.hh>
#include <shaped-graphics-language/driver/impl/front_end.hh>
#include <shaped-shader-library/shader_asset.hh>

// SGL's interpreter as a third opinion: the same corpus entry point and inputs, run on the abstract machine.

using namespace cc::primitive_defines;

namespace
{
/// Every setup's module, once each, as the library a corpus file is checked against.
struct setup_library
{
    cc::vector<cc::string> names;
    cc::vector<cc::string> sources;
    cc::vector<sgl::library_file> files;
    cc::vector<cc::string> problems;
};

[[nodiscard]] setup_library const& setup_modules()
{
    static auto const library = []
    {
        auto result = setup_library();
        for (auto const& s : sg_diff::setups())
        {
            auto const name = s.module.empty() ? cc::format("diff_{}.sgl", s.name) : cc::format("{}.sgl", s.module);
            auto is_read = false;
            for (auto const& known : result.names)
                is_read = is_read || known == name;
            if (is_read)
                continue;
            auto const path = cc::format("{}/{}", nx::source_path(SG_DIFFERENTIAL_SETUPS_DIR), name);
            auto file = cc::file_read_stream_adapter::open(path);
            auto bytes = file.has_value() ? file.value().stream().read_all() : cc::error("cannot open it");
            if (bytes.has_error())
            {
                result.problems.push_back(cc::format("{}: {}", path, bytes.error().to_string()));
                continue;
            }
            result.names.push_back(name);
            result.sources.push_back(
                cc::string(cc::string_view(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size())));
        }
        // the views go in once every string has its final address
        for (auto i = isize(0); i < result.names.size(); ++i)
            result.files.push_back({.name = result.names[i], .source = result.sources[i]});
        return result;
    }();
    return library;
}

/// Sets `v`'s first `parts.size()` scalars, each of the kind its type gives it.
void fill(sgl::check::value& v, cc::span<i64 const> parts)
{
    for (auto i = isize(0); i < parts.size() && i < v.leaves.size(); ++i)
        v.leaves[i] = v.leaves[i].kind == sgl::check::value_kind::scalar_uint
                        ? sgl::check::scalar::of_uint(u32(parts[i]))
                        : sgl::check::scalar::of(i32(parts[i]));
}

/// A pixel stage run over the half's square target, one 2 by 2 quad at a time, so its derivatives are a quad's (EVAL-144).
/// Each pixel's result is its scalars' bits, rows top to bottom and tightly packed, as a target reads back.
cc::result<sg_diff::run_result> pixels_interpreted(sg_diff::workload const& w,
                                                   sgl::check::checked_module const& m,
                                                   sgl::check::flat_entry_point const& e,
                                                   sg_diff::interpreter_half const& half)
{
    using namespace sgl::check;
    if (half.pixel_varyings == nullptr || half.extent <= 0 || half.extent % 2 != 0)
        return sg_diff::run_result{.skipped = "its setup's interpreter half gives no target of whole quads"};

    auto const leaves_per_pixel = leaf_count_of(m, e.result);
    auto texels = cc::vector<u32>::create_filled(half.extent * half.extent * leaves_per_pixel, 0u);
    auto scalars = cc::vector<f32>::create_filled(leaf_count_of(m, e.input), 0.0f);
    for (auto qy = isize(0); qy < half.extent; qy += 2)
        for (auto qx = isize(0); qx < half.extent; qx += 2)
        {
            auto inputs = resolve_inputs(m, e, half.bindings);
            if (inputs.has_error())
                return cc::error(cc::format("{} {}: {}", w.file, w.entry, inputs.error().to_string()));
            inputs.value().lane_count = 4;
            // EVAL-141: lane x + 2y of the quad stands at (x, y)
            for (auto lane = 0; lane < 4; ++lane)
            {
                half.pixel_varyings(f32(qx + lane % 2) + 0.5f, f32(qy + lane / 2) + 0.5f, scalars);
                auto v = zero_value(m, e.input);
                for (auto k = isize(0); k < v.leaves.size(); ++k)
                    v.leaves[k] = scalar::of(scalars[k]);
                inputs.value().lane_parameters.push_back(cc::move(v));
            }

            auto const o = interpret(m, e, inputs.value());
            if (o.status != run_status::ok)
                return cc::error(cc::format("{} {}: the quad at ({}, {}) ends {}: {}", w.file, w.entry, qx, qy,
                                            to_string(o.status), o.detail));
            if (!o.failures.empty())
                return cc::error(cc::format("{} {}: a check fails in the quad at ({}, {})", w.file, w.entry, qx, qy));
            for (auto lane = 0; lane < 4; ++lane)
            {
                auto const pixel = (qy + lane / 2) * half.extent + qx + lane % 2;
                auto const& leaves = o.lane_results[lane].leaves;
                for (auto k = isize(0); k < leaves_per_pixel && k < leaves.size(); ++k)
                    texels[pixel * leaves_per_pixel + k] = leaves[k].bits;
            }
        }

    auto result = sg_diff::run_result();
    result.outputs.push_back({.name = half.outputs.empty() ? cc::string("color") : half.outputs[0].name,
                              .bytes = cc::vector<byte>::create_copy_of(cc::span<u32 const>(texels).as_bytes())});
    return result;
}
} // namespace

cc::result<sg_diff::run_result> sg_diff::interpreted(workload const& w, sg::context& ctx)
{
    using namespace sgl::check;
    if (w.setup->interpreted == nullptr)
        return run_result{.skipped = cc::format("the {} setup has no interpreter half yet", w.setup->name)};
    auto const& library = setup_modules();
    if (!library.problems.empty())
        return cc::error(library.problems[0]);
    auto const source = w.asset->read_source();
    if (!source.has_value())
        return cc::error(cc::format("{}: the library cannot read it", w.file));

    auto const front = sgl::driver::impl::run_front_end(source.value(), w.file, library.files);
    if (!front.errors.empty())
        return cc::error(cc::format("{} does not check: {}", w.file, front.errors));
    auto const& m = front.module;
    auto const* e = static_cast<flat_entry_point const*>(nullptr);
    for (auto const& candidate : m.entry_points)
        if (candidate.name == w.entry)
            e = &candidate;
    if (e == nullptr)
        return cc::error(cc::format("{} has no entry point {}", w.file, w.entry));
    for (auto const b : e->bindings)
        if (m.bindings[m.at(b).info].is_workgroup)
            return run_result{.skipped = "its workgroup memory is shared by invocations the interpreter runs one at a "
                                         "time"};

    auto half = w.setup->interpreted(ctx);
    if (half.skipped.has_value())
        return run_result{.skipped = half.skipped};
    if (e->entry_stage == stage::pixel)
        return pixels_interpreted(w, m, *e, half);
    auto const width = i64(e->workgroup[0] > 0 ? e->workgroup[0] : 1);
    for (auto i = i64(0); i < i64(half.threads); ++i)
    {
        auto inputs = resolve_inputs(m, *e, half.bindings);
        if (inputs.has_error())
            return cc::error(cc::format("{} {}: {}", w.file, w.entry, inputs.error().to_string()));
        inputs.value().stage_inputs.clear();
        for (auto const& si : e->stage_inputs)
        {
            auto v = zero_value(m, e->at(si.local).type);
            switch (si.input)
            {
            case stage_input::thread_id:
                fill(v, cc::span<i64 const>({i, i64(0), i64(0)}));
                break;
            case stage_input::local_thread_id:
                fill(v, cc::span<i64 const>({i % width, i64(0), i64(0)}));
                break;
            case stage_input::local_thread_index:
                fill(v, cc::span<i64 const>({i % width}));
                break;
            case stage_input::workgroup_id:
                fill(v, cc::span<i64 const>({i / width, i64(0), i64(0)}));
                break;
            default:
                return run_result{.skipped = "it reads a stage input the interpreter half does not give"};
            }
            inputs.value().stage_inputs.push_back(cc::move(v));
        }

        auto const o = interpret(m, *e, inputs.value());
        if (o.status != run_status::ok)
            return cc::error(
                cc::format("{} {}: invocation {} ends {}: {}", w.file, w.entry, i, to_string(o.status), o.detail));
        if (!o.failures.empty())
            return cc::error(cc::format("{} {}: a check fails in invocation {}", w.file, w.entry, i));
        write_back(m, o, half.bindings);
    }

    auto result = run_result();
    for (auto const& out : half.outputs)
        result.outputs.push_back({.name = out.name, .bytes = cc::vector<byte>::create_copy_of(out.bytes)});
    return result;
}
