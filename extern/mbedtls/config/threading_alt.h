#pragma once

// The mutex type Mbed TLS uses when MBEDTLS_THREADING_ALT is set.
//
// ALT rather than upstream's MBEDTLS_THREADING_PTHREAD, because pthreads is one platform of the several this repo
// builds for -- Windows has none, and upstream ships no Win32 variant.
// One alternative implementation, supplied by clean-net, serves every target instead.
//
// Deliberately opaque: this header is compiled as C by Mbed TLS and the implementation behind it is C++, so the
// mutex itself cannot appear here.

typedef struct mbedtls_threading_mutex_t
{
    /// The real mutex, owned by the init/free pair clean-net installs.
    /// Null until it is initialized, which is how the free function stays safe to call twice.
    void* opaque;
} mbedtls_threading_mutex_t;
