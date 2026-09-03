cmake_minimum_required(VERSION 3.28)
project(clean-net LANGUAGES CXX)

# Networking for shaped-core: sockets and datagrams, HTTP and WebSocket over a backend seam, and a loopback dev server.
# See readme.md and docs/structure.md.
#
# The transport and the protocol clients are PEERS rather than layers, because a browser has no sockets and does have
# HTTP -- and wasm is a tier-1 platform. So each piece reports its own availability rather than the whole library
# disappearing where one of them cannot exist.
add_library(clean-net
    src/clean-net/fwd.cc
    src/clean-net/common/error.cc
    src/clean-net/common/level.cc
    src/clean-net/common/clock.cc
    src/clean-net/common/cancel.cc
    src/clean-net/address/ip_address.cc
    src/clean-net/address/endpoint.cc
    src/clean-net/address/resolver.cc
    src/clean-net/io/io_system.cc
    src/clean-net/impl/async_glue.cc
    src/clean-net/impl/native_resolve.cc
    src/clean-net/impl/trust_store.cc
    src/clean-net/tls/tls_mbedtls.cc
    src/clean-net/transport/tcp.cc
    src/clean-net/transport/connect.cc
    src/clean-net/http/http_target.cc
    src/clean-net/http/message.cc
    src/clean-net/http/impl/http1.cc
    src/clean-net/http/connection_pool.cc
    src/clean-net/http/http_client.cc
    src/clean-net/http/polite_client.cc
    src/clean-net/transport/virtual_transport.cc
    src/clean-net/transport/simulated_transport.cc
    src/clean-net/impl/native_socket.cc
    src/clean-net/impl/reactor.cc
    src/clean-net/impl/reactor_poll.cc
)

# Public headers live co-located in src/ for better editor experience.
# FILE_SET models the public API explicitly (install/export, IDEs, etc.).
target_sources(clean-net
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/clean-net/fwd.hh
    src/clean-net/all.hh
    src/clean-net/common/error.hh
    src/clean-net/common/level.hh
    src/clean-net/common/deadline.hh
    src/clean-net/common/cancel.hh
    src/clean-net/common/clock.hh
    src/clean-net/address/ip_address.hh
    src/clean-net/address/endpoint.hh
    src/clean-net/address/resolver.hh
    src/clean-net/io/io_system.hh
    src/clean-net/transport/backend.hh
    src/clean-net/transport/stream.hh
    src/clean-net/transport/connect.hh
    src/clean-net/tls/tls.hh
    src/clean-net/http/http_target.hh
    src/clean-net/http/message.hh
    src/clean-net/http/connection_pool.hh
    src/clean-net/http/http_client.hh
    src/clean-net/http/polite_client.hh
    src/clean-net/transport/virtual_transport.hh
    src/clean-net/transport/simulated_transport.hh
)

# Require at least C++23 for this target; the final -std is chosen by the top-level project.
target_compile_features(clean-net
    PUBLIC
    cxx_std_23
)

# Expose "src" so '#include <clean-net/...>' works for consumers.
target_include_directories(clean-net
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(clean-net PUBLIC clean-core)

# Sockets: present everywhere except wasm, which has none of any kind -- not a socket that needs a flag, none at all.
# CNET_HAS_SOCKETS stays PRIVATE and never leaves impl/native_socket.cc; the API stays declared either way and
# cnet::impl::sockets_are_supported() reports the answer at runtime.
if(EMSCRIPTEN)
    target_compile_definitions(clean-net PRIVATE CNET_HAS_SOCKETS=0)
else()
    target_compile_definitions(clean-net PRIVATE CNET_HAS_SOCKETS=1)
endif()

# Winsock is a separate library on Windows and part of libc everywhere else.
# crypt32 is the Windows trust store -- CertOpenSystemStore and its neighbours.
if(WIN32)
    target_link_libraries(clean-net PRIVATE ws2_32 crypt32)
endif()

# The Apple trust store lives in Security.framework, and its data types in CoreFoundation.
# Only the trust store reaches them: the handshake itself is Mbed TLS on every platform.
if(APPLE)
    target_link_libraries(clean-net PRIVATE "-framework CoreFoundation" "-framework Security")
endif()

# TLS: Mbed TLS everywhere but wasm, where the browser holds the TLS and a program never sees a handshake.
# CNET_HAS_TLS stays PRIVATE and never leaves tls_mbedtls.cc; the API stays declared either way and
# cnet::tls_is_supported() reports the answer at runtime.
if(EMSCRIPTEN)
    target_compile_definitions(clean-net PRIVATE CNET_HAS_TLS=0)
else()
    target_compile_definitions(clean-net PRIVATE CNET_HAS_TLS=1)
    target_link_libraries(clean-net PRIVATE mbedtls)
endif()

# Enable conforming preprocessor for MSVC
target_compile_options(clean-net
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

if(SC_BUILD_TESTS)
    add_executable(clean-net-test
        tests/main.cc
        tests/ip-address-test.cc
        tests/endpoint-test.cc
        tests/resolver-test.cc
        tests/clock-test.cc
        tests/error-test.cc
        tests/cancel-test.cc
        tests/native-socket-test.cc
        tests/reactor-test.cc
        tests/io-system-test.cc
        tests/tcp-test.cc
        tests/connect-test.cc
        tests/tls-test.cc
        tests/http-target-test.cc
        tests/http1-test.cc
        tests/http-client-test.cc
        tests/http-politeness-test.cc
        tests/virtual-transport-test.cc
        tests/simulated-transport-test.cc
    )

    target_link_libraries(clean-net-test PRIVATE clean-net nexus)

    sc_add_nexus_web_runner(clean-net-test)
endif()
