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
    src/clean-net/address/ip_address.cc
    src/clean-net/address/endpoint.cc
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
    src/clean-net/common/clock.hh
    src/clean-net/address/ip_address.hh
    src/clean-net/address/endpoint.hh
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
        tests/clock-test.cc
        tests/error-test.cc
    )

    target_link_libraries(clean-net-test PRIVATE clean-net nexus)

    sc_add_nexus_web_runner(clean-net-test)
endif()
