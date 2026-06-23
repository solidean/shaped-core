cmake_minimum_required(VERSION 3.28)
project(clean-core LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# dependencies
# - clean core has no dependencies

# Define the library and its sources
add_library(clean-core
    src/clean-core/common/assert.cc
    src/clean-core/common/hash.cc
    src/clean-core/platform/native.cc
    src/clean-core/memory/allocation.cc
    src/clean-core/memory/node_allocation.cc
    src/clean-core/error/result.cc
    src/clean-core/string/to_string.cc
    src/clean-core/string/string.cc
    src/clean-core/string/string_view.cc
    src/clean-core/container/vector.cc
)

# Public headers live co-located in src/ for better editor experience.
# FILE_SET models the public API explicitly (install/export, IDEs, etc.).
target_sources(clean-core
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/clean-core/fwd.hh

    src/clean-core/common/macros.hh
    src/clean-core/common/utility.hh
    src/clean-core/common/flags.hh
    src/clean-core/common/hash.hh
    src/clean-core/common/assert.hh
    src/clean-core/common/assert-handler.hh
    src/clean-core/common/assertf.hh
    src/clean-core/common/asserts.hh

    src/clean-core/platform/native.hh
    src/clean-core/platform/source_location.hh
    src/clean-core/platform/stacktrace.hh

    src/clean-core/math/bit.hh

    src/clean-core/memory/allocation.hh
    src/clean-core/memory/node_allocation.hh
    src/clean-core/memory/impl/object_lifetime_util.hh

    src/clean-core/container/array.hh
    src/clean-core/container/fixed_array.hh
    src/clean-core/container/unique_array.hh
    src/clean-core/container/vector.hh
    src/clean-core/container/fixed_vector.hh
    src/clean-core/container/unique_vector.hh
    src/clean-core/container/map.hh
    src/clean-core/container/set.hh
    src/clean-core/container/ringbuffer.hh
    src/clean-core/container/bitset.hh
    src/clean-core/container/fixed_bitset.hh
    src/clean-core/container/disjoint_set.hh
    src/clean-core/container/pair.hh
    src/clean-core/container/tuple.hh
    src/clean-core/container/variant.hh
    src/clean-core/container/span.hh
    src/clean-core/container/strided_span.hh
    src/clean-core/container/impl/allocating_container.hh

    src/clean-core/sequence/sequence.hh

    src/clean-core/string/string.hh
    src/clean-core/string/string_view.hh
    src/clean-core/string/char_predicates.hh
    src/clean-core/string/to_string.hh
    src/clean-core/string/to_debug_string.hh

    src/clean-core/function/function_ref.hh
    src/clean-core/function/unique_function.hh

    src/clean-core/error/result.hh
    src/clean-core/error/optional.hh

    src/clean-core/thread/mutex.hh
)

# Libraries should not set a global C++ standard here.
# Instead, declare a minimum on the target so parents stay in control.
# Require at least C++23 for this target; the final -std is chosen by the top-level project.
target_compile_features(clean-core
    PUBLIC
    cxx_std_23
)

# Expose "src" so '#include <clean-core/dummy.hh>' works for consumers.
target_include_directories(clean-core
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# Enable conforming preprocessor for MSVC
target_compile_options(clean-core
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

# Define build configuration macros
target_compile_definitions(clean-core
    PUBLIC
    $<$<CONFIG:Debug>:CC_DEBUG>
    $<$<CONFIG:Release>:CC_RELEASE>
    $<$<CONFIG:RelWithDebInfo>:CC_RELWITHDEBINFO>
)

# Detect and link the correct library for std::stacktrace support
include(DetectStacktraceLib)
cc_detect_stacktrace_link(_CC_STACKTRACE_LIB)

if(_CC_STACKTRACE_LIB STREQUAL "NOTFOUND")
    message(FATAL_ERROR "std::stacktrace: no working link library found")
elseif(NOT _CC_STACKTRACE_LIB STREQUAL "")
    target_link_libraries(clean-core PRIVATE ${_CC_STACKTRACE_LIB})
endif()

# Detect and use the fastest available linker (mold > lld > system default)
include(DetectLinker)
cc_detect_linker(_CC_LINKER)

if(NOT _CC_LINKER STREQUAL "" AND NOT _CC_LINKER STREQUAL "msvc")
    target_link_options(clean-core PUBLIC -fuse-ld=${_CC_LINKER})
endif()

# Test executable
add_executable(clean-core-test
    tests/main.cc
    tests/allocation-test.cc
    tests/array-test.cc
    tests/assert-test.cc
    tests/bit-test.cc
    tests/fixed-array-test.cc
    tests/function_ref-test.cc
    tests/invocable-test.cc
    tests/macros-test.cc
    tests/mutex-test.cc
    tests/node_allocation-test.cc
    tests/optional-test.cc
    tests/result-test.cc
    tests/span-test.cc
    tests/strided_span-test.cc
    tests/string-test.cc
    tests/string_view-test.cc
    tests/to_debug_string-test.cc
    tests/unique_function-test.cc
    tests/utility-test.cc
    tests/vector-test.cc
)

target_link_libraries(clean-core-test
    PRIVATE
    clean-core
    nexus
)

# Coverage
option(CC_COVERAGE_MSVC "Enable MSVC native coverage" OFF)

if(CC_COVERAGE_MSVC AND MSVC)
    # Debug info is required for sensible mapping
    target_compile_options(clean-core PRIVATE /Zi /Od)
    target_compile_options(clean-core-test PRIVATE /Zi /Od)

    # THIS is the critical bit to enable coverage
    target_link_options(clean-core-test PRIVATE /PROFILE)

    # Optional but helps avoid weirdness
    target_link_options(clean-core-test PRIVATE /DEBUG:FULL /INCREMENTAL:NO)
endif()

# Configuration diagnostics
option(CC_PRINT_CONFIG "Print clean-core configuration diagnostics" ON)

if(CC_PRINT_CONFIG)
    # Linker display name
    if(_CC_LINKER STREQUAL "")
        set(_cc_linker_name "system default")
    else()
        set(_cc_linker_name "${_CC_LINKER}")
    endif()

    message(STATUS "")
    message(STATUS "[clean-core] Configuration:")
    if(CMAKE_C_COMPILER_ID)
        message(STATUS "  C compiler   : ${CMAKE_C_COMPILER} (${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION})")
    endif()
    message(STATUS "  C++ compiler : ${CMAKE_CXX_COMPILER} (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION})")
    message(STATUS "  OS           : ${CMAKE_SYSTEM_NAME}")
    message(STATUS "  Linker       : ${_cc_linker_name}")
    message(STATUS "")
endif()
