cmake_minimum_required(VERSION 3.28)
project(mbedtls LANGUAGES C)

# Vendored Mbed TLS (see vendor-mbedtls.py for the pinned version, and for what is dropped).
# One target rather than upstream's three (mbedcrypto / mbedx509 / mbedtls): they are always linked together here,
# and the split exists for embedded builds that take only the crypto half.
set(MBEDTLS_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Globbed rather than listed: 109 sources that change wholesale on a version bump, where a hand-maintained list would
# be a merge conflict per bump and a link error when it drifts.
# CONFIGURE_DEPENDS so a re-vendor is picked up without a manual cmake run.
file(GLOB MBEDTLS_SOURCES CONFIGURE_DEPENDS ${MBEDTLS_DIR}/library/*.c)

add_library(mbedtls STATIC ${MBEDTLS_SOURCES})

# SYSTEM so upstream's headers do not trip our warnings in the consuming TU.
# config/ is on the PUBLIC path because the user config below is included by name from mbedtls_config.h.
target_include_directories(mbedtls SYSTEM PUBLIC
    ${MBEDTLS_DIR}/include
    ${MBEDTLS_DIR}/config
)

# library/ holds internal headers the sources reach by quote include; they resolve relative to the including file,
# so this is only needed for the few that do not.
target_include_directories(mbedtls PRIVATE ${MBEDTLS_DIR}/library)

# PUBLIC, and it must stay that way.
# Several of the macros in that file change struct layouts, so a consumer compiling mbedtls headers without it would
# disagree with this library about what an mbedtls_ssl_context is -- which links cleanly and crashes at run time.
target_compile_definitions(mbedtls PUBLIC MBEDTLS_USER_CONFIG_FILE="shaped_mbedtls_config.h")

# Third-party code: silence its warnings, hide its symbols (statically absorbed).
if (MSVC)
    target_compile_options(mbedtls PRIVATE /W0)
else()
    target_compile_options(mbedtls PRIVATE -w -fvisibility=hidden)
endif()

set_property(TARGET mbedtls PROPERTY POSITION_INDEPENDENT_CODE ON)
