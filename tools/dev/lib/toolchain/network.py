"""Networking-environment checks: what cnet's TLS and HTTP backends actually resolve to on this machine.

Every check here is advisory, never a failure.
The repo configures, builds and passes its whole suite with none of it, because clean-net degrades rather than
disappearing: a build with no TLS still declares `cnet::tls_connect`, and `cnet::tls_is_supported()` answers at
runtime instead.
That is what makes a gap invisible until an HTTPS request fails for a reason nobody expected — naming the cost is the
whole point of these lines.

Each check mirrors one thing the build already decides, and the mirror must stay exact:
the `CNET_HAS_TLS` gate in libs/base/clean-net/CMakeLists.txt, the vendored pin in extern/mbedtls/dependency.yml, and
the backend ladder in libs/base/clean-net/docs/structure.md.
"""

from __future__ import annotations

import ctypes.util
import re
from pathlib import Path


def _mbedtls_check(root: Path) -> tuple[str, bool | None, str]:
    """Which TLS backend cnet was built with, and which version of it.

    Vendored rather than fetched, so this reports the version rather than presence: a checkout that has it missing is
    a broken checkout, not a machine without a feature.
    """
    label = "TLS backend"
    header = root / "extern" / "mbedtls" / "include" / "mbedtls" / "build_info.h"

    if not header.is_file():
        return (label, False, "extern/mbedtls is missing — the vendored sources are not optional")

    text = header.read_text(encoding="utf-8", errors="replace")
    match = re.search(r'#define\s+MBEDTLS_VERSION_STRING\s+"([^"]+)"', text)
    version = match.group(1) if match else "version unreadable"
    return (label, True, f"Mbed TLS {version} (vendored)")


def _tls_platform_check() -> tuple[str, bool | None, str]:
    """Whether this platform can enumerate its own trust anchors, which is the harder half of TLS.

    Windows, macOS and Linux read the machine's roots; iOS and Android report `unsupported` rather than an empty set,
    because neither can enumerate anchors at all and both want the OS to evaluate a chain instead.
    """
    import platform as _platform

    label = "TLS trust store"
    system = _platform.system()

    if system == "Windows":
        return (label, True, "the ROOT system store")
    if system == "Darwin":
        return (label, True, "SecTrustSettings, over the three domains")
    if system == "Linux":
        # The same probe order the trust store itself walks; the first that exists is the one it reads.
        bundles = (
            "/etc/ssl/certs/ca-certificates.crt",
            "/etc/pki/tls/certs/ca-bundle.crt",
            "/etc/ssl/ca-bundle.pem",
            "/etc/ssl/cert.pem",
        )
        found = next((b for b in bundles if Path(b).is_file()), None)
        if found is not None:
            return (label, True, found)
        return (label, None, "no CA bundle at any known path — install the distribution's ca-certificates package")

    return (label, None, f"{system or 'this platform'} has no anchor enumeration; a chain is evaluated by the OS")


def _libcurl_check() -> tuple[str, bool | None, str]:
    """Whether a system libcurl is present, which is the HTTP backend cnet would `dlopen` where one exists.

    Not built yet, so this reports the machine rather than the build: it is what says whether that backend would have
    anything to bind to once it is written.
    """
    label = "system libcurl"
    found = ctypes.util.find_library("curl")
    if found:
        return (label, True, f"{found} — available to a future backend; nothing binds it yet")
    return (label, None, "not found — the native backend is the only one either way today")


def _http_backends_check(root: Path, is_wasm: bool) -> tuple[str, bool | None, str]:
    """Which HTTP backends this build compiled in, so the automatic choice is inspectable rather than inferred.

    A capability ladder rather than a bag of booleans: code written against a level works on every backend at that
    level or above, and `cnet::http_client::level()` is what a caller asks.
    """
    label = "HTTP backends"

    if not (root / "libs" / "base" / "clean-net").is_dir():
        return (label, None, "clean-net is not in this checkout")

    if is_wasm:
        # A browser has no sockets and does have `fetch`, which is the whole reason the transport and the protocol
        # clients are peers rather than layers.
        return (label, None, "wasm: `fetch` is the only possible backend, and it is not written yet")

    return (label, True, "native (our own transport, HTTP/1.1)")


def checks(root: Path, is_wasm: bool) -> list[tuple[str, bool | None, str]]:
    """The networking environment, as (label, ok, detail) triples in the order doctor prints them.

    `is_wasm` is what separates "no sockets here, and never will be" from "not on this machine" — the same
    distinction `cnet::error_code::unsupported` draws, and the reason a caller decides once at startup rather than
    probing per call.
    """
    return [
        _mbedtls_check(root),
        _tls_platform_check(),
        _libcurl_check(),
        _http_backends_check(root, is_wasm),
    ]
