# cpp-style: rules about how C++ is spelled in this tree.
#
# Every file a rule owns is named here except its corpus .md, which the test binary discovers at run time.
# The two targets are created by the parent CMakeLists, so a group only appends to them.

target_sources(shaped-linter-core PRIVATE
    default-init-assignment/default_init_assignment.cc
    qualified-primitive/qualified_primitive.cc
)

if(SC_BUILD_TESTS)
    target_sources(shaped-linter-test PRIVATE
        default-init-assignment/default_init_assignment-test.cc
        qualified-primitive/qualified_primitive-test.cc
    )
endif()
