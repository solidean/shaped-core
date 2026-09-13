#include <clean-core/common/profiling.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-shader-compiler-dxc/compiler.hh>
#include <shaped-shader-compiler-dxc/impl/command_line_args.hh>
#include <shaped-shader-compiler-dxc/impl/dxc_common.hh>
#include <shaped-shader-compiler-dxc/impl/include_handler.hh>
#include <shaped-shader-compiler-dxc/impl/reflection.hh>

#include <memory>
#include <vector>

namespace ssc::dxc
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "ssc.dxc");

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
/// Called with the DXC lock held; see it.
[[nodiscard]] cc::result<dxc_invocation> invoke_dxc_locked(IDxcUtils* utils,
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

// ---- DXC's process-global state ------------------------------------------------------------------
//
// The vendored libdxcompiler carries state of its own, BELOW the IDxcCompiler3 instance.
// ThreadSanitizer catches it directly: two concurrent compiles, one per instance and one per thread exactly as
// compiler.hh prescribes, allocate in one thread and free in the other through DXC's own WideCharToMultiByte shim.
// So "one compiler per thread" does not buy what it says it does, and this lock is what actually holds today.
//
// It is a holding position rather than an answer -- the finding is unresolved, and
// libs/graphics/shaped-shader-compiler-dxc/docs/thread-safety.md is the write-up: what was observed, what is still
// unknown, and what a real fix would look like.
// Flip this to 0 to get the un-serialized behaviour back for that investigation.
#define SSC_DXC_SERIALIZE_INVOCATIONS 1

#if SSC_DXC_SERIALIZE_INVOCATIONS
cc::mutex<cc::unit> g_dxc_lock;
#endif

/// Runs `f` with DXC's global state to itself.
///
/// Every entry into libdxcompiler goes through here, and "entry" is wider than it looks.
/// Creating an instance, extracting an output blob, building a reflection and RELEASING any of those COM pointers are
/// each a call into the library, so the whole of `preprocess`, `compile`, `create` and `~compiler` is inside rather
/// than the `Compile` call alone.
/// The lock is not recursive, so everything under it calls `invoke_dxc_locked` rather than re-entering here.
template <class F>
auto with_dxc_serialized(F&& f)
{
#if SSC_DXC_SERIALIZE_INVOCATIONS
    return g_dxc_lock.lock([&](cc::unit&) { return cc::invoke(f); });
#else
    return cc::invoke(f);
#endif
}
} // namespace

cc::result<compiler> compiler::create()
{
    // The lambda yields the state rather than a compiler, so no `compiler` is ever constructed or destroyed while the
    // lock is held -- `~compiler` takes the same lock, and it is not recursive.
    // The failure paths still release inside it: `i` dies there, carrying whichever instance was created.
    auto created = with_dxc_serialized(
        [&]() -> cc::result<std::unique_ptr<state>>
        {
            auto i = std::make_unique<state>();
            if (HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(i->utils.GetAddressOf())); FAILED(hr))
                return impl::dxc_error(hr, "DxcCreateInstance(DxcUtils)");
            if (HRESULT hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(i->compiler.GetAddressOf())); FAILED(hr))
                return impl::dxc_error(hr, "DxcCreateInstance(DxcCompiler)");
            i->version = query_version(i->compiler.Get());
            return cc::move(i);
        });
    CC_RETURN_IF_ERROR(created);

    return compiler(cc::move(created.value()));
}

compiler::~compiler()
{
    // A moved-from compiler owns nothing, and taking a process-wide lock to reset a null pointer is also how this
    // destructor would re-enter a lock a caller is already holding.
    if (_state == nullptr)
        return;

    // Releasing the two COM pointers frees memory libdxcompiler allocated, which is exactly the half of the observed
    // race that is a free -- so teardown is serialized like every other entry.
    with_dxc_serialized([this] { _state.reset(); });
}

cc::string_view compiler::version() const
{
    return _state->version;
}

cc::result<preprocessed_source> compiler::preprocess(shader_description const& desc,
                                                     include_resolver resolve_include,
                                                     compile_options const& options)
{
    CC_RECORD_SCOPE("ssc.preprocess");

    auto args = impl::build_preprocess_args(desc, options);
    CC_RETURN_IF_ERROR(args);

    // Everything below is a call into libdxcompiler -- the include handler, the compile, the output blob, and the
    // release of every COM pointer on the way out -- so the lock spans all of it rather than the compile alone.
    return with_dxc_serialized(
        [&]() -> cc::result<preprocessed_source>
        {
            impl::ComPtr<IDxcIncludeHandler> handler = impl::make_include_handler(_state->utils.Get(), resolve_include);
            auto invocation = invoke_dxc_locked(_state->utils.Get(), _state->compiler.Get(), desc.source, args.value(),
                                                handler.Get(), "shader preprocess failed");
            CC_RETURN_IF_ERROR(invocation);
            IDxcResult* result = invocation.value().result.Get();

            impl::ComPtr<IDxcBlob> hlsl;
            if (HRESULT hr = result->GetOutput(DXC_OUT_HLSL, IID_PPV_ARGS(hlsl.GetAddressOf()), nullptr);
                FAILED(hr) || !hlsl)
                return impl::dxc_error(hr, "GetOutput(DXC_OUT_HLSL)");

            preprocessed_source out;
            out.source
                = cc::string(reinterpret_cast<char const*>(hlsl->GetBufferPointer()), isize(hlsl->GetBufferSize()));
            out.warnings = impl::dxc_diagnostics(result);
            return out;
        });
}

cc::result<sg::compiled_shader> compiler::compile(shader_description const& desc, compile_options const& options)
{
    // The most expensive thing this library does by a wide margin, and the reason the cache in front of it exists.
    CC_RECORD_SCOPE("ssc.compile");

    auto args = impl::build_compile_args(desc, options);
    CC_RETURN_IF_ERROR(args);

    // Same span as preprocess, and reflection is the reason it matters here: IDxcUtils::CreateReflection is as much a
    // call into libdxcompiler as Compile is.
    return with_dxc_serialized(
        [&]() -> cc::result<sg::compiled_shader>
        {
            // Reject includes: compile() takes already-preprocessed source.
            impl::ComPtr<IDxcIncludeHandler> reject = impl::make_reject_include_handler();
            auto invocation = invoke_dxc_locked(_state->utils.Get(), _state->compiler.Get(), desc.source, args.value(),
                                                reject.Get(), "shader compilation failed");
            CC_RETURN_IF_ERROR(invocation);
            IDxcResult* result = invocation.value().result.Get();

            impl::ComPtr<IDxcBlob> object;
            if (HRESULT hr = result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(object.GetAddressOf()), nullptr);
                FAILED(hr) || !object || object->GetBufferSize() == 0)
                return cc::error("shaped-shader-compiler-dxc: DXC produced no object bytecode");

            // Reflection comes from wherever the target puts it: DXIL carries it in a container beside the bytecode,
            // SPIR-V carries it in the module itself.
            auto const object_bytes = cc::span<byte const>(static_cast<byte const*>(object->GetBufferPointer()),
                                                           isize(object->GetBufferSize()));
            auto reflected = options.target == compile_target::spirv
                               ? impl::reflect_spirv(object_bytes, desc.stage, desc.entry_point)
#ifdef CC_OS_WINDOWS
                               : impl::reflect(_state->utils.Get(), result, desc.stage, desc.entry_point);
#else
                               : cc::result<impl::reflected_shader>(
                                     cc::error("DXIL reflection needs the Windows SDK's d3d12shader.h, which the Linux "
                                               "DXC release does not ship — compile to SPIR-V instead"));
#endif
            CC_RETURN_IF_ERROR(reflected);

            sg::compiled_shader shader;
            shader.stage = desc.stage;
            shader.format = options.target == compile_target::spirv ? sg::shader_format::spirv : sg::shader_format::dxil;
            shader.entry_point = desc.entry_point;
            auto const bytes = cc::span<byte const>(reinterpret_cast<byte const*>(object->GetBufferPointer()),
                                                    isize(object->GetBufferSize()));
            shader.bytecode = cc::make_pinned_data(bytes);
            shader.bindings = cc::move(reflected.value().bindings);
            // Reflection reports what the shader declares, never which stage it was compiled for, so the stage is
            // stamped here — the one place that knows it.
            // merge_bindings then unions the stages as a pipeline's shaders are folded into one layout.
            sg::apply_stage_visibility(shader.bindings, shader.stage);
            shader.workgroup_size = reflected.value().workgroup_size;
            shader.compiler = sg::compiler_info{
                .name = cc::string("dxc"),
                .version = _state->version,
                .signature = impl::join_args(args.value()),
            };
            return shader;
        });
}
} // namespace ssc::dxc
