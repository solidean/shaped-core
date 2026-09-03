#include "trust_store.hh"

#include <clean-core/string/format.hh>

// One adapter per platform, and nothing shared between them but the shape of the answer.
//
// The roots are read once per handshake rather than cached, deliberately.
// A machine's trust store changes while a program runs -- an enterprise policy push, a developer trusting a proxy's
// certificate -- and a cache would mean a program has to be restarted before it believes what its owner already
// decided.
// Reading a few hundred certificates costs about a millisecond, against a handshake that costs tens.

#if defined(_WIN32)

// clang-format off
// windows.h brings a `byte` typedef of its own into the global namespace, from rpcndr.h, and it collides with the
// one every other file here writes bare.
// Renaming theirs for the length of their own headers is the smallest fix: a typedef cannot be #undef'd, our headers
// above are already parsed, and nothing in the Windows API spells that name in lower case anyway.
#define byte win32_rpcndr_byte
// windows.h must precede wincrypt.h, which is declared in terms of it.
#include <windows.h>
#include <wincrypt.h>
#undef byte
// clang-format on

namespace cnet::impl
{
cc::result<cc::vector<cc::vector<byte>>, error> system_root_certificates()
{
    // "ROOT" is the machine's trusted root store, which is the set that answers "is this chain acceptable".
    // Opening it by name rather than by handle is what picks up group policy: the system store is a union of the
    // registry, the local machine and the current user, resolved by the OS.
    auto* const store = ::CertOpenSystemStoreW(0, L"ROOT");
    if (store == nullptr)
        return cc::error(error{.code = error_code::unknown,
                               .native_code = i32(::GetLastError()),
                               .message = cc::string("could not open the Windows root certificate store")});

    auto roots = cc::vector<cc::vector<byte>>();

    PCCERT_CONTEXT context = nullptr;
    while ((context = ::CertEnumCertificatesInStore(store, context)) != nullptr)
    {
        if (context->dwCertEncodingType != X509_ASN_ENCODING || context->pbCertEncoded == nullptr)
            continue;

        auto der = cc::vector<byte>();
        der.resize_to_defaulted(isize(context->cbCertEncoded));
        for (isize i = 0; i < der.size(); ++i)
            der[i] = byte(context->pbCertEncoded[i]);

        roots.push_back(cc::move(der));
    }

    ::CertCloseStore(store, 0);

    if (roots.empty())
        return cc::error(error{.code = error_code::unknown,
                               .native_code = 0,
                               .message = cc::string("the Windows root certificate store is empty")});

    return roots;
}
} // namespace cnet::impl

#else

namespace cnet::impl
{
cc::result<cc::vector<cc::vector<byte>>, error> system_root_certificates()
{
    // Not yet written for this platform, and saying so is the point: an empty list would let a caller believe a
    // connection was verified against a store nobody read.
    // Apple wants SecTrustEvaluateWithError, Linux a probe of the distro PEM paths, Android a trip through JNI.
    // Until then a caller here supplies its own roots, which `tls_trust::additional_roots_pem` is for.
    return cc::error(unsupported_here("the platform trust store"));
}
} // namespace cnet::impl

#endif
