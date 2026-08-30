#pragma once

#include <clean-core/common/assert.hh>

/// A minimal COM smart pointer over the subset of Microsoft::WRL::ComPtr this library uses.
///
/// It exists because WRL is Windows-only and DXC is not.
/// `<wrl/client.h>` ships with MSVC alone, and DXC's own WinAdapter offers `CComPtr` off Windows with a different
/// surface — `operator&` where WRL has `GetAddressOf()`. Conditionally aliasing one to the other would put an #ifdef
/// at every call site; this puts it nowhere, because both platforms get the same type.
///
/// Deliberately not `cc::shared_ptr` or any other clean-core pointer: COM objects carry their own intrusive refcount
/// reached through IUnknown, so ownership here is AddRef/Release rather than a control block.

namespace ssc::dxc::impl
{
template <class T>
class com_ptr
{
public:
    com_ptr() = default;
    ~com_ptr() { reset(); }

    com_ptr(com_ptr const& rhs) : _p(rhs._p)
    {
        if (_p != nullptr)
            _p->AddRef();
    }
    com_ptr& operator=(com_ptr const& rhs)
    {
        if (this != &rhs)
        {
            auto* const old = _p;
            _p = rhs._p;
            if (_p != nullptr)
                _p->AddRef();
            if (old != nullptr)
                old->Release();
        }
        return *this;
    }

    com_ptr(com_ptr&& rhs) noexcept : _p(rhs._p) { rhs._p = nullptr; }
    com_ptr& operator=(com_ptr&& rhs) noexcept
    {
        if (this != &rhs)
        {
            reset();
            _p = rhs._p;
            rhs._p = nullptr;
        }
        return *this;
    }

    /// The raw interface pointer, without transferring ownership.
    [[nodiscard]] T* Get() const { return _p; }

    /// Where a COM call writes its out-parameter.
    /// Asserts on a non-empty pointer: overwriting one leaks the reference it held, and every call site here is a
    /// fresh acquisition rather than a reassignment.
    [[nodiscard]] T** GetAddressOf()
    {
        CC_ASSERT(_p == nullptr, "GetAddressOf on a non-empty com_ptr would leak its reference");
        return &_p;
    }

    /// Releases ownership without releasing the reference, for handing it to a C API that takes it.
    [[nodiscard]] T* Detach()
    {
        auto* const p = _p;
        _p = nullptr;
        return p;
    }

    void reset()
    {
        if (_p != nullptr)
        {
            _p->Release();
            _p = nullptr;
        }
    }

    T* operator->() const { return _p; }
    explicit operator bool() const { return _p != nullptr; }

private:
    T* _p = nullptr;
};
} // namespace ssc::dxc::impl
