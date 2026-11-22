add_library(__lib__
    src/__lib__/greeting.cc
)
target_sources(__lib__
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/__lib__/fwd.hh
    src/__lib__/greeting.hh
)
target_compile_features(__lib__ PUBLIC cxx_std_23)
target_include_directories(__lib__ PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>")
target_link_libraries(__lib__ PUBLIC clean-core)

add_executable(__lib__-test
    tests/main.cc
    tests/greeting-test.cc
)
sc_nexus_binary(__lib__-test KINDS tests)
target_link_libraries(__lib__-test PRIVATE __lib__ nexus)
