#include <shaped-shader-compiler-dxc/impl/include_handler.hh>
#include <wrl/implements.h>

namespace ssc::dxc::impl
{
namespace
{
/// Delegates DXC's include loads to the caller-supplied resolver (a virtual file system: embedded
/// resources, in-memory sources, etc.). Returns E_FAIL when the resolver can't find the path, which
/// DXC surfaces as an include error.
class resolver_include_handler final
  : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDxcIncludeHandler>
{
public:
    resolver_include_handler(IDxcUtils* utils, include_resolver resolver) : _utils(utils), _resolver(resolver) {}

    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR filename, IDxcBlob** include_source) noexcept override
    {
        if (include_source == nullptr)
            return E_POINTER;
        *include_source = nullptr;

        cc::string const path = from_wide(filename);
        cc::optional<cc::string> resolved = _resolver(path);
        if (!resolved.has_value())
            return E_FAIL;

        ComPtr<IDxcBlobEncoding> blob;
        if (HRESULT hr = _utils->CreateBlob(resolved.value().data(), UINT32(resolved.value().size()), DXC_CP_UTF8,
                                            blob.GetAddressOf());
            FAILED(hr))
            return hr;
        *include_source = blob.Detach();
        return S_OK;
    }

private:
    IDxcUtils* _utils;
    include_resolver _resolver;
};

/// Fails every include — installed during compile() so a stray `#include` in supposedly-preprocessed
/// source is a hard error rather than a silent filesystem read.
class reject_include_handler final
  : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDxcIncludeHandler>
{
public:
    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR, IDxcBlob** include_source) noexcept override
    {
        if (include_source != nullptr)
            *include_source = nullptr;
        return E_FAIL;
    }
};
} // namespace

ComPtr<IDxcIncludeHandler> make_include_handler(IDxcUtils* utils, include_resolver resolve_include)
{
    return Microsoft::WRL::Make<resolver_include_handler>(utils, resolve_include);
}

ComPtr<IDxcIncludeHandler> make_reject_include_handler()
{
    return Microsoft::WRL::Make<reject_include_handler>();
}
} // namespace ssc::dxc::impl
