#include "trust_store.hh"

#include <clean-core/string/format.hh>

// One adapter per platform, and nothing shared between them but the shape of the answer.
//
// The roots are read once per handshake rather than cached, deliberately.
// A machine's trust store changes while a program runs -- an enterprise policy push, a developer trusting a proxy's
// certificate -- and a cache would mean a program has to be restarted before it believes what its owner already
// decided.
// Reading a few hundred certificates costs about a millisecond, against a handshake that costs tens.
//
// WHAT APPLE AND ANDROID NEED THAT THIS SHAPE CANNOT GIVE THEM.
// iOS has no public API to enumerate anchors at all, and Android's live behind JNI.
// The supported path on both is to hand a built chain to the OS and let IT decide -- SecTrustEvaluateWithError, or
// the Java trust manager -- which is a verify callback rather than a set of roots.
// That is a different seam from this one, and it is the shape those two will need when their turn comes.

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
cc::result<system_roots, error> system_root_certificates()
{
    // "ROOT" is the machine's trusted root store, which is the set that answers "is this chain acceptable".
    // Opening it by name rather than by handle is what picks up group policy: the system store is a union of the
    // registry, the local machine and the current user, resolved by the OS.
    auto* const store = ::CertOpenSystemStoreW(0, L"ROOT");
    if (store == nullptr)
        return cc::error(error{.code = error_code::unknown,
                               .native_code = i32(::GetLastError()),
                               .message = cc::string("could not open the Windows root certificate store")});

    auto roots = system_roots();

    PCCERT_CONTEXT context = nullptr;
    while ((context = ::CertEnumCertificatesInStore(store, context)) != nullptr)
    {
        if (context->dwCertEncodingType != X509_ASN_ENCODING || context->pbCertEncoded == nullptr)
            continue;

        auto der = cc::vector<byte>();
        der.resize_to_defaulted(isize(context->cbCertEncoded));
        for (isize i = 0; i < der.size(); ++i)
            der[i] = byte(context->pbCertEncoded[i]);

        roots.der.push_back(cc::move(der));
    }

    ::CertCloseStore(store, 0);

    if (roots.empty())
        return cc::error(error{.code = error_code::unknown,
                               .native_code = 0,
                               .message = cc::string("the Windows root certificate store is empty")});

    return roots;
}
} // namespace cnet::impl

#elif defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <TargetConditionals.h>

namespace cnet::impl
{
#if TARGET_OS_OSX

namespace
{
/// Read one domain's anchors into `roots`, and say whether the domain could be read at all.
///
/// The three domains are a union rather than a fallback: system holds what Apple ships, admin what an administrator
/// added, and user what this user trusted -- and a corporate proxy's certificate lives in one of the latter two.
bool append_domain(system_roots& roots, SecTrustSettingsDomain domain)
{
    CFArrayRef certificates = nullptr;
    if (::SecTrustSettingsCopyCertificates(domain, &certificates) != errSecSuccess || certificates == nullptr)
        return false;

    auto const count = ::CFArrayGetCount(certificates);
    for (CFIndex i = 0; i < count; ++i)
    {
        auto* const value = const_cast<void*>(::CFArrayGetValueAtIndex(certificates, i));
        auto const certificate = static_cast<SecCertificateRef>(value);
        if (certificate == nullptr)
            continue;

        auto const data = ::SecCertificateCopyData(certificate);
        if (data == nullptr)
            continue;

        auto const* const bytes = ::CFDataGetBytePtr(data);
        auto const length = ::CFDataGetLength(data);

        auto der = cc::vector<byte>();
        der.resize_to_defaulted(isize(length));
        for (isize k = 0; k < der.size(); ++k)
            der[k] = byte(bytes[k]);

        roots.der.push_back(cc::move(der));
        ::CFRelease(data);
    }

    ::CFRelease(certificates);
    return true;
}
} // namespace

cc::result<system_roots, error> system_root_certificates()
{
    auto roots = system_roots();

    auto any_domain_read = append_domain(roots, kSecTrustSettingsDomainSystem);
    any_domain_read = append_domain(roots, kSecTrustSettingsDomainAdmin) || any_domain_read;
    any_domain_read = append_domain(roots, kSecTrustSettingsDomainUser) || any_domain_read;

    if (!any_domain_read)
        return cc::error(error{.code = error_code::unknown,
                               .native_code = 0,
                               .message = cc::string("no macOS trust settings domain could be read")});

    if (roots.empty())
        return cc::error(error{.code = error_code::unknown,
                               .native_code = 0,
                               .message = cc::string("the macOS trust settings hold no certificates")});

    return roots;
}

#else

cc::result<system_roots, error> system_root_certificates()
{
    // iOS has no public API to enumerate anchors, on purpose: the supported path is to hand the OS a chain and let
    // it decide, which is a verify callback rather than a set of roots.
    // Reporting `unsupported` is the honest answer until that seam exists.
    return cc::error(unsupported_here("the iOS trust store"));
}

#endif // TARGET_OS_OSX
} // namespace cnet::impl

#elif defined(__linux__)

#include <clean-core/error/optional.hh>
#include <clean-core/streams/file_stream.hh>

namespace cnet::impl
{
namespace
{
/// Where the distributions keep their CA bundle, most common first.
///
/// Probed rather than assumed: the path varies by distribution and there is no standard, which is exactly the kind
/// of per-platform knowledge a hand-rolled trust store gets wrong on somebody else's machine.
constexpr cc::string_view k_bundle_paths[] = {
    "/etc/ssl/certs/ca-certificates.crt", // Debian, Ubuntu, Alpine, Arch
    "/etc/pki/tls/certs/ca-bundle.crt",   // Fedora, RHEL, CentOS
    "/etc/ssl/ca-bundle.pem",             // openSUSE
    "/etc/pki/tls/cacert.pem",            // older RHEL
    "/etc/ssl/cert.pem",                  // Alpine, and anything BSD-flavoured
};

/// Read a whole file, or nothing if it is not there.
[[nodiscard]] cc::optional<cc::string> read_whole_file(cc::string_view path)
{
    auto adapter = cc::file_read_stream_adapter::open(path);
    if (adapter.has_error())
        return {};

    auto stream = adapter.value().stream();

    auto const content = stream.read_all();
    if (content.has_error() || content.value().empty())
        return {};

    return cc::string(cc::string_view(reinterpret_cast<char const*>(content.value().data()), content.value().size()));
}
} // namespace

cc::result<system_roots, error> system_root_certificates()
{
    auto roots = system_roots();

    // The first bundle that exists wins: they are the same set under different names, and reading several would hand
    // the parser every root twice.
    for (auto const path : k_bundle_paths)
        if (auto bundle = read_whole_file(path); bundle.has_value())
        {
            roots.pem.push_back(cc::move(bundle.value()));
            return roots;
        }

    return cc::error(error{.code = error_code::unknown,
                           .native_code = 0,
                           .message = cc::string("no CA bundle was found in any of the usual places")});
}
} // namespace cnet::impl

#else

namespace cnet::impl
{
cc::result<system_roots, error> system_root_certificates()
{
    // Android's roots live behind JNI, and wasm has no handshake here at all.
    // Saying so is the point: an empty list would let a caller believe a connection was verified against a store
    // nobody read, so a caller here supplies its own through `tls_trust::additional_roots_pem`.
    return cc::error(unsupported_here("the platform trust store"));
}
} // namespace cnet::impl

#endif
