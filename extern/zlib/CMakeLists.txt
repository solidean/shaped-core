cmake_minimum_required(VERSION 3.28)
project(zlib LANGUAGES C)

# Vendored zlib (see vendor-zlib.py for the pinned version, and for what is dropped).
# Ten TUs: the deflate encoder, the inflate decoder and its tables, the two checksums, and the shared allocator shims.
set(ZLIB_DIR ${CMAKE_CURRENT_SOURCE_DIR})

add_library(zlib STATIC
    ${ZLIB_DIR}/src/adler32.c
    ${ZLIB_DIR}/src/compress.c
    ${ZLIB_DIR}/src/crc32.c
    ${ZLIB_DIR}/src/deflate.c
    ${ZLIB_DIR}/src/inffast.c
    ${ZLIB_DIR}/src/inflate.c
    ${ZLIB_DIR}/src/inftrees.c
    ${ZLIB_DIR}/src/trees.c
    ${ZLIB_DIR}/src/uncompr.c
    ${ZLIB_DIR}/src/zutil.c
)

# SYSTEM so zlib.h does not trip our warnings in the consuming TU.
# include/ holds zlib.h and zconf.h and nothing else, which is exactly the public API;
# the internal headers stay in src/ where nothing outside can name them.
target_include_directories(zlib SYSTEM PUBLIC ${ZLIB_DIR}/include)

# The internal headers reach zlib.h with a quote include. That misses in src/ and then resolves against
# include/ above, so src/ only has to be visible to zlib's own TUs.
target_include_directories(zlib PRIVATE ${ZLIB_DIR}/src)

# The stdio gzFile sources are not compiled, so nothing here opens a FILE*.
# gzguts.h is still vendored, because zutil.c includes it for the allocator declarations its zcalloc/zfree need.
# The gz* prototypes stay declared and undefined; nothing references them, so they never reach the link.
# Z_SOLO is deliberately not defined — it would also strip compress()/uncompress() and force a caller-supplied allocator.

# Third-party code: silence its warnings, hide its symbols (statically absorbed).
if (MSVC)
    target_compile_options(zlib PRIVATE /W0)
else()
    target_compile_options(zlib PRIVATE -w -fvisibility=hidden)
endif()

set_property(TARGET zlib PROPERTY POSITION_INDEPENDENT_CODE ON)
