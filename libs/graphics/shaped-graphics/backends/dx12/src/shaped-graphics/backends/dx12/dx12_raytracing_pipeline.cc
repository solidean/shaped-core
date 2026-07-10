#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/backends/dx12/dx12_pipeline_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_raytracing_pipeline.hh>

#include <cstring>
#include <string>

namespace sg::backend::dx12
{
namespace
{
// Ray-tracing entry-point / export names are ASCII HLSL identifiers, so a byte-wise widen is enough for the
// UTF-16 the DXR subobject descs want.
[[nodiscard]] std::wstring widen(cc::string_view s)
{
    std::wstring w;
    w.reserve(size_t(s.size()));
    for (char c : s)
        w.push_back(wchar_t(static_cast<unsigned char>(c)));
    return w;
}

// One renamed export in a DXIL library: `entry_point` is the original HLSL name, `export_name` the unique
// `export_N` it is renamed to (so the same entry name in two libraries can't collide).
struct lib_export
{
    std::wstring entry_point;
    std::wstring export_name;
};

// A deduplicated DXIL library (one compiled blob), with the exports referenced from it.
struct lib_entry
{
    void const* data = nullptr;
    SIZE_T size = 0;
    cc::vector<lib_export> exports;
};

// A reference to a renamed export within the deduplicated libraries.
struct export_ref
{
    size_t lib_index;
    size_t export_index;
};

// Assembles the deduplicated libraries + export renaming for a set of shaders, so the same blob is emitted
// once and every (blob, entry point) gets a unique `export_N`.
struct library_builder
{
    cc::vector<lib_entry> libraries;
    size_t next_suffix = 0;

    [[nodiscard]] size_t acquire_library(sg::compiled_shader const& shader)
    {
        void const* const key = shader.bytecode.data();
        for (size_t i = 0; i < size_t(libraries.size()); ++i)
            if (libraries[i].data == key)
                return i;
        libraries.push_back(lib_entry{.data = key, .size = SIZE_T(shader.bytecode.size()), .exports = {}});
        return size_t(libraries.size()) - 1;
    }

    [[nodiscard]] export_ref acquire_export(sg::compiled_shader const& shader)
    {
        size_t const lib_index = acquire_library(shader);
        std::wstring const entry = widen(shader.entry_point);
        auto& exports = libraries[cc::isize(lib_index)].exports;
        for (size_t i = 0; i < size_t(exports.size()); ++i)
            if (exports[i].entry_point == entry)
                return {lib_index, i};
        exports.push_back(lib_export{.entry_point = entry,
                                     .export_name = widen(cc::string_view("export_")) + std::to_wstring(next_suffix++)});
        return {lib_index, size_t(exports.size()) - 1};
    }

    [[nodiscard]] wchar_t const* export_name(export_ref ref) const
    {
        return libraries[cc::isize(ref.lib_index)].exports[cc::isize(ref.export_index)].export_name.c_str();
    }
};

// One hit group: its DXR name plus the exports of the present component shaders. `intersection` present ->
// procedural, else triangles.
struct hit_group_build
{
    std::wstring name;
    cc::optional<export_ref> closest_hit;
    cc::optional<export_ref> any_hit;
    cc::optional<export_ref> intersection;
};

[[nodiscard]] cc::result<dx12_raytracing_pipeline::shader_identifier> fetch_identifier(ID3D12StateObjectProperties* props,
                                                                                       wchar_t const* export_name)
{
    void const* const id = props->GetShaderIdentifier(export_name);
    if (id == nullptr)
        return cc::error("ID3D12StateObjectProperties::GetShaderIdentifier returned null");
    dx12_raytracing_pipeline::shader_identifier out;
    std::memcpy(out.bytes, id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    return out;
}
} // namespace

cc::result<dx12_raytracing_pipeline_handle> dx12_raytracing_pipeline::create(ID3D12Device* device,
                                                                             dx12_pipeline_layout_handle layout,
                                                                             sg::raytracing_pipeline_description const& desc)
{
    CC_ASSERT(layout != nullptr, "raytracing pipeline requires a pipeline_layout");
    CC_ASSERT(!desc.raygen_shaders.empty(), "raytracing pipeline requires at least one raygen shader");
    CC_ASSERT(desc.max_recursion_depth >= 1, "max_recursion_depth must be >= 1");

    // Dedup the DXIL libraries and rename their exports. Building refs first keeps the wstring names (pointed
    // at by the D3D12 descs below) alive in `builder` through CreateStateObject.
    library_builder builder;
    cc::vector<export_ref> raygen_refs;
    cc::vector<export_ref> miss_refs;
    cc::vector<export_ref> callable_refs;
    cc::vector<hit_group_build> hit_groups_build;
    raygen_refs.reserve(desc.raygen_shaders.size());
    miss_refs.reserve(desc.miss_shaders.size());
    callable_refs.reserve(desc.callable_shaders.size());
    hit_groups_build.reserve(desc.hit_shaders.size());

    for (auto const& s : desc.raygen_shaders)
        raygen_refs.push_back(builder.acquire_export(s));
    for (auto const& s : desc.miss_shaders)
        miss_refs.push_back(builder.acquire_export(s));
    for (auto const& s : desc.callable_shaders)
        callable_refs.push_back(builder.acquire_export(s));
    for (cc::isize i = 0; i < desc.hit_shaders.size(); ++i)
    {
        auto const& h = desc.hit_shaders[i];
        hit_group_build hg;
        hg.name = widen(cc::string_view("HitGroup_")) + std::to_wstring(size_t(i));
        if (h.closest_hit.has_value())
            hg.closest_hit = builder.acquire_export(h.closest_hit.value());
        if (h.any_hit.has_value())
            hg.any_hit = builder.acquire_export(h.any_hit.value());
        if (h.intersection.has_value())
            hg.intersection = builder.acquire_export(h.intersection.value());
        hit_groups_build.push_back(cc::move(hg));
    }

    // From here `builder.libraries` must not change — the D3D12 descs point into its wstrings and blobs.
    size_t total_exports = 0;
    for (auto const& lib : builder.libraries)
        total_exports += size_t(lib.exports.size());

    // Reserve to exact capacity: `exports` is pointed into by pExports, and `dxil_libraries` / `hit_groups`
    // by the subobjects — a reallocation would dangle those pointers.
    cc::vector<D3D12_EXPORT_DESC> exports;
    exports.reserve(cc::isize(total_exports));
    cc::vector<D3D12_DXIL_LIBRARY_DESC> dxil_libraries;
    dxil_libraries.reserve(builder.libraries.size());
    for (auto const& lib : builder.libraries)
    {
        cc::isize const first_export = exports.size();
        for (auto const& e : lib.exports)
            exports.push_back(D3D12_EXPORT_DESC{.Name = e.export_name.c_str(),
                                                .ExportToRename = e.entry_point.c_str(),
                                                .Flags = D3D12_EXPORT_FLAG_NONE});

        D3D12_DXIL_LIBRARY_DESC lib_desc = {};
        lib_desc.DXILLibrary.pShaderBytecode = lib.data;
        lib_desc.DXILLibrary.BytecodeLength = lib.size;
        lib_desc.NumExports = UINT(lib.exports.size());
        lib_desc.pExports = &exports[first_export];
        dxil_libraries.push_back(lib_desc);
    }

    cc::vector<D3D12_HIT_GROUP_DESC> hit_groups;
    hit_groups.reserve(hit_groups_build.size());
    for (auto const& hg : hit_groups_build)
    {
        wchar_t const* const closest = hg.closest_hit.has_value() ? builder.export_name(hg.closest_hit.value()) : nullptr;
        wchar_t const* const any = hg.any_hit.has_value() ? builder.export_name(hg.any_hit.value()) : nullptr;
        wchar_t const* const intersection
            = hg.intersection.has_value() ? builder.export_name(hg.intersection.value()) : nullptr;

        D3D12_HIT_GROUP_DESC hg_desc = {};
        hg_desc.HitGroupExport = hg.name.c_str();
        hg_desc.Type
            = intersection != nullptr ? D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE : D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hg_desc.AnyHitShaderImport = any;
        hg_desc.ClosestHitShaderImport = closest;
        hg_desc.IntersectionShaderImport = intersection;
        hit_groups.push_back(hg_desc);
    }

    D3D12_RAYTRACING_SHADER_CONFIG shader_config = {};
    shader_config.MaxPayloadSizeInBytes = UINT(desc.max_payload_size);
    shader_config.MaxAttributeSizeInBytes = UINT(desc.max_attribute_size);

    D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config = {};
    pipeline_config.MaxTraceRecursionDepth = desc.max_recursion_depth;

    D3D12_GLOBAL_ROOT_SIGNATURE global_root_signature = {};
    global_root_signature.pGlobalRootSignature = layout->root_signature.Get();

    // A single global shader/pipeline config + root signature applies to every export (no per-shader
    // associations). Order: DXIL libraries, hit groups, shader config, pipeline config, global root signature.
    cc::vector<D3D12_STATE_SUBOBJECT> subobjects;
    subobjects.reserve(dxil_libraries.size() + hit_groups.size() + 3);
    for (auto const& lib_desc : dxil_libraries)
        subobjects.push_back(D3D12_STATE_SUBOBJECT{.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, .pDesc = &lib_desc});
    for (auto const& hg_desc : hit_groups)
        subobjects.push_back(D3D12_STATE_SUBOBJECT{.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, .pDesc = &hg_desc});
    subobjects.push_back(
        D3D12_STATE_SUBOBJECT{.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, .pDesc = &shader_config});
    subobjects.push_back(D3D12_STATE_SUBOBJECT{.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,
                                               .pDesc = &pipeline_config});
    subobjects.push_back(D3D12_STATE_SUBOBJECT{.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,
                                               .pDesc = &global_root_signature});

    D3D12_STATE_OBJECT_DESC state_object_desc = {};
    state_object_desc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    state_object_desc.NumSubobjects = UINT(subobjects.size());
    state_object_desc.pSubobjects = subobjects.data();

    // Query ID3D12Device5 OUTSIDE the assert — its out-param is a real side effect that CC_ASSERT would
    // compile out with asserts off.
    ComPtr<ID3D12Device5> device5;
    [[maybe_unused]] HRESULT const device5_hr = device->QueryInterface(IID_PPV_ARGS(&device5));
    CC_ASSERT(SUCCEEDED(device5_hr) && device5, "ID3D12Device5 unavailable (SDK/driver too old for DXR)");

    auto pipeline = std::make_shared<dx12_raytracing_pipeline>();
    pipeline->layout = cc::move(layout);
    if (HRESULT hr = device5->CreateStateObject(&state_object_desc, IID_PPV_ARGS(&pipeline->state_object)); FAILED(hr))
        return dx12_error(hr, "ID3D12Device5::CreateStateObject failed");

    ComPtr<ID3D12StateObjectProperties> props;
    if (HRESULT hr = pipeline->state_object.As(&props); FAILED(hr))
        return dx12_error(hr, "ID3D12StateObject::QueryInterface(ID3D12StateObjectProperties) failed");

    // Fetch the 32-byte identifiers per handle: raygen/miss/callable by their export name, hit by group name.
    for (auto const& ref : raygen_refs)
    {
        auto id = fetch_identifier(props.Get(), builder.export_name(ref));
        CC_RETURN_IF_ERROR(id);
        pipeline->shader_ids_raygen.push_back(id.value());
    }
    for (auto const& ref : miss_refs)
    {
        auto id = fetch_identifier(props.Get(), builder.export_name(ref));
        CC_RETURN_IF_ERROR(id);
        pipeline->shader_ids_miss.push_back(id.value());
    }
    for (auto const& ref : callable_refs)
    {
        auto id = fetch_identifier(props.Get(), builder.export_name(ref));
        CC_RETURN_IF_ERROR(id);
        pipeline->shader_ids_callable.push_back(id.value());
    }
    for (auto const& hg : hit_groups_build)
    {
        auto id = fetch_identifier(props.Get(), hg.name.c_str());
        CC_RETURN_IF_ERROR(id);
        pipeline->shader_ids_hit.push_back(id.value());
    }

    return dx12_raytracing_pipeline_handle(cc::move(pipeline));
}

cc::pinned_data<cc::byte const> dx12_raytracing_pipeline::cached_pipeline_data() const
{
    // TODO: serialize the state object (ID3D12StateObjectProperties has no GetCachedBlob equivalent; would
    // need collection state objects / add-to-state-object). Empty until then.
    return {};
}
} // namespace sg::backend::dx12
