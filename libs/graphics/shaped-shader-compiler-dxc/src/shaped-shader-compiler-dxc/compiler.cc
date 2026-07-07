#include <clean-core/container/pinned_data.hh>
#include <shaped-shader-compiler-dxc/compiler.hh>
#include <shaped-shader-compiler-dxc/impl/command_line_args.hh>
#include <shaped-shader-compiler-dxc/impl/dxc_common.hh>
#include <shaped-shader-compiler-dxc/impl/include_handler.hh>
#include <shaped-shader-compiler-dxc/impl/reflection.hh>

#include <memory>
#include <vector>

namespace ssc::dxc
{
struct compiler::state
{
    impl::ComPtr<IDxcUtils> utils;
    impl::ComPtr<IDxcCompiler3> compiler;
    cc::string version; ///< DXC version string (best-effort), folded into provenance
};

compiler::compiler(std::unique_ptr<state> s) : _state(cc::move(s))
{
}
compiler::compiler(compiler&&) noexcept = default;
compiler& compiler::operator=(compiler&&) noexcept = default;
compiler::~compiler() = default;

namespace
{
[[nodiscard]] cc::string query_version(IDxcCompiler3* c)
{
    impl::ComPtr<IDxcVersionInfo> vi;
    if (SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(vi.GetAddressOf()))) && vi)
    {
        UINT32 major = 0, minor = 0;
        if (SUCCEEDED(vi->GetVersion(&major, &minor)))
            return cc::string(cc::format("{}.{}", major, minor));
    }
    return {};
}

/// A DXC result behind a struct — WRL ComPtr overloads unary operator&, so it cannot be stored
/// directly in a cc::result (which takes &value internally); a plain struct wrapper sidesteps that.
struct dxc_invocation
{
    impl::ComPtr<IDxcResult> result;
};

/// Compiles `desc.source` with the given argv + include handler, returning the raw DXC result (after
/// checking GetStatus). Shared by preprocess() and compile().
[[nodiscard]] cc::result<dxc_invocation> invoke_dxc(IDxcUtils* utils,
                                                    IDxcCompiler3* dxc,
                                                    cc::string_view source,
                                                    impl::arg_storage const& args,
                                                    IDxcIncludeHandler* include_handler,
                                                    char const* what)
{
    auto src = impl::make_source_blob(utils, source);
    CC_RETURN_IF_ERROR(src);

    std::vector<LPCWSTR> argv;
    argv.reserve(args.size());
    for (auto const& w : args)
        argv.push_back(w.c_str());

    dxc_invocation out;
    if (HRESULT hr = dxc->Compile(&src.value().buffer, argv.data(), UINT32(argv.size()), include_handler,
                                  IID_PPV_ARGS(out.result.GetAddressOf()));
        FAILED(hr))
        return impl::dxc_error(hr, what);

    HRESULT status = S_OK;
    if (SUCCEEDED(out.result->GetStatus(&status)) && FAILED(status))
        return cc::error(cc::format("{}: {}", what, impl::dxc_diagnostics(out.result.Get())));

    return out;
}
} // namespace

cc::result<compiler> compiler::create()
{
    auto i = std::make_unique<state>();
    if (HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(i->utils.GetAddressOf())); FAILED(hr))
        return impl::dxc_error(hr, "DxcCreateInstance(DxcUtils)");
    if (HRESULT hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(i->compiler.GetAddressOf())); FAILED(hr))
        return impl::dxc_error(hr, "DxcCreateInstance(DxcCompiler)");
    i->version = query_version(i->compiler.Get());
    return compiler(cc::move(i));
}

cc::result<preprocessed_source> compiler::preprocess(shader_description const& desc,
                                                     include_resolver resolve_include,
                                                     compile_options const& options)
{
    auto args = impl::build_preprocess_args(desc, options);
    CC_RETURN_IF_ERROR(args);

    impl::ComPtr<IDxcIncludeHandler> handler = impl::make_include_handler(_state->utils.Get(), resolve_include);
    auto invocation = invoke_dxc(_state->utils.Get(), _state->compiler.Get(), desc.source, args.value(), handler.Get(),
                                 "shader preprocess failed");
    CC_RETURN_IF_ERROR(invocation);
    IDxcResult* result = invocation.value().result.Get();

    impl::ComPtr<IDxcBlob> hlsl;
    if (HRESULT hr = result->GetOutput(DXC_OUT_HLSL, IID_PPV_ARGS(hlsl.GetAddressOf()), nullptr); FAILED(hr) || !hlsl)
        return impl::dxc_error(hr, "GetOutput(DXC_OUT_HLSL)");

    preprocessed_source out;
    out.source = cc::string(reinterpret_cast<char const*>(hlsl->GetBufferPointer()), cc::isize(hlsl->GetBufferSize()));
    out.warnings = impl::dxc_diagnostics(result);
    return out;
}

cc::result<sg::compiled_shader> compiler::compile(shader_description const& desc, compile_options const& options)
{
    auto args = impl::build_compile_args(desc, options);
    CC_RETURN_IF_ERROR(args);

    // Reject includes: compile() takes already-preprocessed source.
    impl::ComPtr<IDxcIncludeHandler> reject = impl::make_reject_include_handler();
    auto invocation = invoke_dxc(_state->utils.Get(), _state->compiler.Get(), desc.source, args.value(), reject.Get(),
                                 "shader compilation failed");
    CC_RETURN_IF_ERROR(invocation);
    IDxcResult* result = invocation.value().result.Get();

    impl::ComPtr<IDxcBlob> object;
    if (HRESULT hr = result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(object.GetAddressOf()), nullptr);
        FAILED(hr) || !object || object->GetBufferSize() == 0)
        return cc::error("shaped-shader-compiler-dxc: DXC produced no object bytecode");

    auto reflected = impl::reflect(_state->utils.Get(), result, desc.stage);
    CC_RETURN_IF_ERROR(reflected);

    sg::compiled_shader shader;
    shader.stage = desc.stage;
    shader.format = sg::shader_format::dxil;
    shader.entry_point = desc.entry_point;
    auto const bytes = cc::span<cc::byte const>(reinterpret_cast<cc::byte const*>(object->GetBufferPointer()),
                                                cc::isize(object->GetBufferSize()));
    shader.bytecode = cc::make_pinned_data(bytes);
    shader.bindings = cc::move(reflected.value().bindings);
    shader.workgroup_size = reflected.value().workgroup_size;
    shader.compiler = sg::compiler_info{
        .name = cc::string("dxc"),
        .version = _state->version,
        .signature = impl::join_args(args.value()),
    };
    return shader;
}
} // namespace ssc::dxc
