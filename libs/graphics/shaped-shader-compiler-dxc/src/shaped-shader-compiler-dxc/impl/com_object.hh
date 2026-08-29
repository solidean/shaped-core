#pragma once

#include <clean-core/thread/atomic.hh>
#include <shaped-shader-compiler-dxc/impl/dxc_common.hh>

/// A minimal IUnknown implementation for the COM objects this library hands to DXC.
///
/// The WRL equivalent, `Microsoft::WRL::RuntimeClass`, ships with MSVC only, and the two include handlers below are
/// the whole of what needs it — so implementing the three IUnknown methods directly costs less than finding a
/// portable class framework, and keeps the handlers reading as ordinary C++.
///
/// Reference counting rather than a clean-core pointer, because that is what the COM ABI is: DXC calls AddRef and
/// Release on the raw interface, and takes ownership of what LoadSource hands back.

namespace ssc::dxc::impl
{
template <class Interface>
class com_object : public Interface
{
public:
    // Born with one reference, which the com_ptr the factory returns takes over.
    com_object() = default;

    com_object(com_object const&) = delete;
    com_object& operator=(com_object const&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) noexcept override
    {
        if (out == nullptr)
            return E_POINTER;
        *out = nullptr;
        if (riid == __uuidof(Interface) || riid == __uuidof(IUnknown))
        {
            *out = static_cast<Interface*>(this);
            this->AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ULONG(_references.fetch_add(1) + 1); }

    ULONG STDMETHODCALLTYPE Release() noexcept override
    {
        auto const remaining = _references.fetch_sub(1) - 1;
        if (remaining == 0)
            delete this;
        return ULONG(remaining);
    }

protected:
    // Only Release destroys one, which is what makes `delete this` above the single owner of the lifetime.
    virtual ~com_object() = default;

private:
    cc::atomic<int> _references = 1;
};
} // namespace ssc::dxc::impl
