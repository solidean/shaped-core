#include <clean-core/record/log.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-shader-compiler-dxc/compiler.hh>

using namespace cc::primitive_defines;

// Whether D3D12 takes a DXIL container no validator signed, which decides whether a writer of our own needs `dxil.dll`.
// Manual, since it asks one question of whatever runtime this build loads and pins nothing: run it by name, read its log.
// libs/graphics/shaped-graphics-language/docs/issues/epic-sgl-native-ir-targets.md holds the answer it gave.

namespace
{
constexpr auto k_digest_offset = isize(4);
constexpr auto k_digest_size = isize(16);

[[nodiscard]] HRESULT pipeline_of(ID3D12Device* device, ID3D12RootSignature* root, cc::span<byte const> shader)
{
    auto const desc = D3D12_COMPUTE_PIPELINE_STATE_DESC{
        .pRootSignature = root,
        .CS = {.pShaderBytecode = shader.data(), .BytecodeLength = SIZE_T(shader.size())}};
    ID3D12PipelineState* pipeline = nullptr;
    auto const result = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
    if (pipeline != nullptr)
        pipeline->Release();
    return result;
}
} // namespace

TEST("dxil signing spike - which containers D3D12 takes", nx::config::manual)
{
    auto compiler = ssc::dxc::compiler::create();
    REQUIRE(compiler.has_value());
    constexpr auto source
        = "RWStructuredBuffer<uint> values : register(u0);\n"
          "[numthreads(4, 1, 1)] void main(uint3 id : SV_DispatchThreadID) { values[id.x] = id.x; }\n";
    auto signed_shader = compiler.value().compile({.source = source}, {});
    REQUIRE(signed_shader.has_value());
    auto unsigned_shader = compiler.value().compile({.source = source}, {.extra_args = {cc::string("-Vd")}});
    REQUIRE(unsigned_shader.has_value());

    ID3D12Device* device = nullptr;
    REQUIRE(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))));

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER parameter = {};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    D3D12_ROOT_SIGNATURE_DESC described = {};
    described.NumParameters = 1;
    described.pParameters = &parameter;
    ID3DBlob* blob = nullptr;
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&described, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)));
    ID3D12RootSignature* root = nullptr;
    REQUIRE(
        SUCCEEDED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root))));

    auto const report = [&](cc::string_view name, cc::span<byte const> shader)
    {
        auto digest = cc::string();
        for (auto i = k_digest_offset; i < k_digest_offset + 4; ++i)
            digest.appendf("{:02x}", u32(u8(shader[i])));
        CC_LOG_INFO("{}: digest {}..., CreateComputePipelineState {:#x}", name, digest,
                    u32(pipeline_of(device, root, shader)));
    };
    auto const stamped = [&](u8 value)
    {
        auto copy = cc::vector<byte>::create_copy_of(signed_shader.value().bytecode.span());
        for (auto i = k_digest_offset; i < k_digest_offset + k_digest_size; ++i)
            copy[i] = byte(value);
        return copy;
    };

    report("signed by dxil.dll", signed_shader.value().bytecode.span());
    report("-Vd, never validated", unsigned_shader.value().bytecode.span());
    report("its digest cleared", stamped(0));
    report("the retail bypass digest, all 0x01", stamped(1));
    report("the preview bypass digest, all 0x02", stamped(2));
    auto tampered = cc::vector<byte>::create_copy_of(signed_shader.value().bytecode.span());
    tampered[tampered.size() - 8] = byte(u8(tampered[tampered.size() - 8]) ^ 1);
    report("a signed digest over a changed byte", tampered);

    root->Release();
    blob->Release();
    device->Release();
}
