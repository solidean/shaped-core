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
/// Every setup's module, `diff_<setup>.sgl`, as the library a corpus file is checked against.
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
            auto const name = cc::format("diff_{}.sgl", s.name);
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
        v.leaves[i] = v.leaves[i].kind == sgl::check::value_kind::scalar_uint ? sgl::check::scalar::of_uint(u32(parts[i]))
                                                                             : sgl::check::scalar::of(i32(parts[i]));
}
} // namespace

cc::result<sg_diff::run_result> sg_diff::interpreted(workload const& w)
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
            return run_result{.skipped = "its workgroup memory is shared by invocations the interpreter runs one at a time"};

    auto half = w.setup->interpreted();
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
            return cc::error(cc::format("{} {}: invocation {} ends {}: {}", w.file, w.entry, i, to_string(o.status), o.detail));
        if (!o.failures.empty())
            return cc::error(cc::format("{} {}: a check fails in invocation {}", w.file, w.entry, i));
        write_back(m, o, half.bindings);
    }

    auto result = run_result();
    result.outputs.push_back({.name = half.output, .bytes = cc::vector<byte>::create_copy_of(half.output_bytes)});
    return result;
}
