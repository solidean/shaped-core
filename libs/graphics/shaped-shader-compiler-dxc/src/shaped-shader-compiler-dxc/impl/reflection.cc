#include <shaped-shader-compiler-dxc/impl/reflection.hh>

namespace ssc::dxc::impl
{
namespace
{
/// Maps a reflected resource binding onto the sg binding_type with the matching (view_class, view_shape).
/// nullopt for kinds sg has no vocabulary for yet — the caller turns that into an error naming the
/// resource, so growing sg::binding_type is a deliberate, visible step. The `Dimension` distinguishes a
/// texture from a typed/texel buffer: both a `Texture2D` and a `Buffer<T>` reflect as a TEXTURE / RWTYPED
/// kind, but the typed-buffer case reports a BUFFER dimension, which sg has no texel-buffer view for yet.
[[nodiscard]] cc::optional<sg::binding_type> map_binding_type(D3D12_SHADER_INPUT_BIND_DESC const& bd)
{
    switch (bd.Type)
    {
    case D3D_SIT_CBUFFER:
        return sg::binding_type::uniform_buffer;
    case D3D_SIT_STRUCTURED:
        return sg::binding_type::readonly_structured_buffer;
    case D3D_SIT_BYTEADDRESS:
        return sg::binding_type::readonly_raw_buffer;
    case D3D_SIT_UAV_RWSTRUCTURED:
        return sg::binding_type::readwrite_structured_buffer;
    case D3D_SIT_UAV_RWBYTEADDRESS:
        return sg::binding_type::readwrite_raw_buffer;
    case D3D_SIT_SAMPLER:
        return sg::binding_type::sampler;
    case D3D_SIT_TEXTURE:
        if (bd.Dimension == D3D_SRV_DIMENSION_BUFFER)
            return {}; // Buffer<T> — a typed/texel buffer, not a texture
        return sg::binding_type::readonly_texture;
    case D3D_SIT_UAV_RWTYPED:
        if (bd.Dimension == D3D_SRV_DIMENSION_BUFFER)
            return {}; // RWBuffer<T> — a typed/texel buffer, not a storage texture
        return sg::binding_type::readwrite_texture;
    case D3D_SIT_RTACCELERATIONSTRUCTURE:
        return sg::binding_type::acceleration_structure; // RaytracingAccelerationStructure (SRV, VA-addressed)
    default:
        return {};
    }
}
} // namespace

cc::result<reflected_shader> reflect(IDxcUtils* utils, IDxcResult* result, sg::shader_stage stage)
{
    ComPtr<IDxcBlob> reflection_blob;
    if (HRESULT hr = result->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(reflection_blob.GetAddressOf()), nullptr);
        FAILED(hr) || !reflection_blob)
        return dxc_error(hr, "GetOutput(DXC_OUT_REFLECTION)");

    DxcBuffer const buffer = {reflection_blob->GetBufferPointer(), reflection_blob->GetBufferSize(), 0};
    ComPtr<ID3D12ShaderReflection> reflection;
    if (HRESULT hr = utils->CreateReflection(&buffer, IID_PPV_ARGS(reflection.GetAddressOf())); FAILED(hr))
        return dxc_error(hr, "IDxcUtils::CreateReflection");

    D3D12_SHADER_DESC shader_desc = {};
    if (HRESULT hr = reflection->GetDesc(&shader_desc); FAILED(hr))
        return dxc_error(hr, "ID3D12ShaderReflection::GetDesc");

    reflected_shader out;
    out.bindings.reserve(cc::isize(shader_desc.BoundResources));
    for (UINT i = 0; i < shader_desc.BoundResources; ++i)
    {
        D3D12_SHADER_INPUT_BIND_DESC bd = {};
        if (HRESULT hr = reflection->GetResourceBindingDesc(i, &bd); FAILED(hr))
            return dxc_error(hr, "ID3D12ShaderReflection::GetResourceBindingDesc");

        cc::optional<sg::binding_type> const type = map_binding_type(bd);
        if (!type.has_value())
            return cc::error(cc::format("shaped-shader-compiler-dxc: resource '{}' is a binding kind shaped-graphics "
                                        "does not model yet (texel / typed buffers and append/consume/counter buffers "
                                        "are not supported)",
                                        bd.Name));

        // Faithful (register, space, kind) -> (index, set, type). No remapping; see reflection.hh.
        sg::binding b;
        b.name = cc::string(bd.Name);
        b.set = bd.Space;
        b.index = bd.BindPoint;
        b.count = bd.BindCount;
        b.type = type.value();

        if (bd.Type == D3D_SIT_CBUFFER)
        {
            if (ID3D12ShaderReflectionConstantBuffer* cb = reflection->GetConstantBufferByName(bd.Name); cb != nullptr)
            {
                D3D12_SHADER_BUFFER_DESC cb_desc = {};
                if (SUCCEEDED(cb->GetDesc(&cb_desc)))
                    b.block_size = cc::isize(cb_desc.Size);
            }
        }

        out.bindings.push_back(cc::move(b));
    }

    if (stage == sg::shader_stage::compute)
    {
        UINT x = 0, y = 0, z = 0;
        reflection->GetThreadGroupSize(&x, &y, &z);
        out.workgroup_size = sg::compute_dimensions{.x = int(x), .y = int(y), .z = int(z)};
    }

    return out;
}
} // namespace ssc::dxc::impl
