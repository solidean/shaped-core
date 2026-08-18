# Example binaries for nexus `EXAMPLE` declarations: the `*-example` targets under a library's `examples/`
# folder, or under the repo-root `examples/<category>/` tree for a cross-library one.
#
# An example is built by every normal and CI build and run by nobody automatically; `dev.py example <match>`
# runs exactly one. docs/guides/examples.md is the concept, nexus/test.hh the EXAMPLE macro.
#
#   sc_add_example(<target> SOURCES <file>... [MAIN <file>] [LINK <lib>...] [PCH <tier>...])
#       One binary from one or more sources. Without MAIN, a main.cc calling nx::run is generated.
#
#   sc_add_single_file_examples(<prefix> FILES <file>... [LINK <lib>...] [PCH <tier>...])
#       One binary per listed file, each with its own generated main, named `<prefix>-<stem>-example`.
#       This is what lets one CMakeLists define a dozen single-file examples with no subdirectory clutter.
#
# Both are no-ops when SC_BUILD_EXAMPLES is off, so a call site needs no `if()` around it.

# The generated main, byte for byte what every hand-written tests/main.cc holds.
set(SC_EXAMPLE_MAIN_CONTENT
"#include <nexus/run.hh>

int main(int argc, char** argv) { return nx::run(argc, argv); }
"
    CACHE INTERNAL "Body of the generated example main.cc")

function(sc_add_example target)
    if(NOT SC_BUILD_EXAMPLES)
        return()
    endif()

    cmake_parse_arguments(PARSE_ARGV 1 EX "" "MAIN" "SOURCES;LINK;PCH")

    if(NOT EX_SOURCES)
        message(FATAL_ERROR "sc_add_example(${target}): SOURCES is required")
    endif()
    if(EX_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "sc_add_example(${target}): unexpected arguments: ${EX_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT target MATCHES "-example$")
        message(FATAL_ERROR "sc_add_example(${target}): an example target must be named '*-example' — that suffix is how dev.py tells examples from tests")
    endif()

    # file(CONFIGURE), not file(WRITE): copy-if-different, so a configure that changes nothing leaves the
    # mtime alone rather than recompiling and relinking every example on every preset `dev.py check` builds.
    # Written at configure time rather than by add_custom_command, which would bind the helper to the
    # directory scope it is called from.
    if(EX_MAIN)
        set(_main "${EX_MAIN}")
    else()
        set(_main "${CMAKE_CURRENT_BINARY_DIR}/examples/${target}/main.cc")
        file(CONFIGURE OUTPUT "${_main}" CONTENT "${SC_EXAMPLE_MAIN_CONTENT}" @ONLY)
    endif()

    add_executable(${target} "${_main}" ${EX_SOURCES})
    target_link_libraries(${target} PRIVATE nexus ${EX_LINK})

    if(EX_PCH)
        sc_target_pch(${target} ${EX_PCH})
    endif()

    # Stage the shared libraries an example links, so it starts when run from its own build directory.
    # A test binary that needs this does it by hand; an example is always launched directly, so the helper owes it.
    #
    # The whole command sits inside the genex rather than only its arguments: TARGET_RUNTIME_DLLS is empty for most
    # examples, and `cmake -E copy_if_different` with no sources is an error rather than a no-op.
    # An empty COMMAND is skipped instead.
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:${target}>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:${target}>;$<TARGET_FILE_DIR:${target}>>"
            COMMAND_EXPAND_LISTS
        )
    endif()
endfunction()

function(sc_add_single_file_examples prefix)
    if(NOT SC_BUILD_EXAMPLES)
        return()
    endif()

    cmake_parse_arguments(PARSE_ARGV 1 EX "" "" "FILES;LINK;PCH")

    if(NOT EX_FILES)
        message(FATAL_ERROR "sc_add_single_file_examples(${prefix}): FILES is required")
    endif()
    if(EX_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "sc_add_single_file_examples(${prefix}): unexpected arguments: ${EX_UNPARSED_ARGUMENTS}")
    endif()

    foreach(_file IN LISTS EX_FILES)
        get_filename_component(_stem "${_file}" NAME_WE)
        sc_add_example("${prefix}-${_stem}-example"
            SOURCES "${_file}"
            LINK ${EX_LINK}
            PCH ${EX_PCH}
        )
    endforeach()
endfunction()
