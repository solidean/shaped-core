# prose: rules about the sentences we write, wherever they sit.
#
# These are the cross-language rules — a comment, a docstring and a markdown paragraph are the same thing
# to them — so they are grouped by what they are about rather than by the language they run on.

target_sources(shaped-linter-core PRIVATE
    no-flow-prose/no_flow_prose.cc
)

if(SC_BUILD_TESTS)
    target_sources(shaped-linter-test PRIVATE
        no-flow-prose/no_flow_prose-test.cc
    )
endif()
