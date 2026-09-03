#pragma once

// shaped-core's adjustments to Mbed TLS's default configuration.
//
// This is a USER config: Mbed TLS includes it AFTER include/mbedtls/mbedtls_config.h, so everything here is a
// deliberate subtraction from upstream's defaults rather than a configuration written from scratch.
// Starting from the default and removing is the safer direction -- a hand-written config that forgets a module fails
// at link time, while one that forgets a dependency of a module fails during a handshake against one peer in ten.
//
// It is passed as MBEDTLS_USER_CONFIG_FILE from CMakeLists.txt, PUBLIC and not PRIVATE, and it must stay that way:
// several of these macros change struct layouts, so a consumer compiling mbedtls headers without them would disagree
// with the library about what an mbedtls_ssl_context is.
//
// WHAT IS REMOVED, AND WHY.
// Everything here is a piece of the operating system that clean-net already owns, or a piece of upstream we do not
// call.
// Nothing cryptographic is turned off: cipher suites, curves and protocol versions stay at upstream's defaults,
// because a narrowed set is a compatibility bug that only shows up against the one server nobody tested with.

// Upstream's own BSD-socket layer.
// clean-net drives the record layer over its own transport, which is what lets TLS run over a virtual network with
// no sockets at all -- and the reason this must be off rather than merely unused: mbedtls_net_* would otherwise be
// the second place in this library that knows what a socket is.
#undef MBEDTLS_NET_C

// Upstream's timing and alarm helpers, which read the clock themselves through gettimeofday and select.
// `cnet::clock` is the seam every deadline in this library is measured against, and a second time source that a test
// cannot move would undo that.
#undef MBEDTLS_TIMING_C

// Loading certificates and keys from files.
// clean-net hands over PEM buffers -- the roots come from the platform trust store as bytes, never as a path -- so
// the file loaders are surface with no caller.
#undef MBEDTLS_FS_IO

// Persistent PSA key storage, which is those same file loaders under another name.
// Both go together: the storage layer is written in terms of the ITS one, so removing one without the other does not
// compile.
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C

// Upstream's self-test entry points, which nothing here calls.
// They are a meaningful amount of code and constant data per module, all of it reachable only from mbedtls_*_self_test.
#undef MBEDTLS_SELF_TEST
