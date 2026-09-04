#include "mbedtls_threading.hh"

#ifndef CNET_HAS_TLS
#define CNET_HAS_TLS 0
#endif

#if CNET_HAS_TLS

#include <mbedtls/threading.h>

#include <mutex>

namespace cnet::impl
{
namespace
{
void mbedtls_mutex_init(mbedtls_threading_mutex_t* mutex)
{
    mutex->opaque = new std::mutex();
}

void mbedtls_mutex_free(mbedtls_threading_mutex_t* mutex)
{
    delete static_cast<std::mutex*>(mutex->opaque);
    mutex->opaque = nullptr;
}

int mbedtls_mutex_lock(mbedtls_threading_mutex_t* mutex)
{
    if (mutex->opaque == nullptr)
        return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    static_cast<std::mutex*>(mutex->opaque)->lock();
    return 0;
}

int mbedtls_mutex_unlock(mbedtls_threading_mutex_t* mutex)
{
    if (mutex->opaque == nullptr)
        return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    static_cast<std::mutex*>(mutex->opaque)->unlock();
    return 0;
}
} // namespace

void ensure_mbedtls_threading()
{
    // A function-local static is what makes "exactly once" true across threads; `mbedtls_threading_set_alt` itself is
    // not safe to call twice or concurrently, and it must run before the first context is initialized because it is
    // what brings upstream's own global mutexes to life.
    static auto const installed = []
    {
        mbedtls_threading_set_alt(mbedtls_mutex_init, mbedtls_mutex_free, mbedtls_mutex_lock, mbedtls_mutex_unlock);
        return true;
    }();
    (void)installed;
}
} // namespace cnet::impl

#else

namespace cnet::impl
{
void ensure_mbedtls_threading()
{
}
} // namespace cnet::impl

#endif // CNET_HAS_TLS
