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

#   sc_resolve_example_backend(<out_backend> <out_mode> SUPPORTS <dx12|vulkan>...)
#       Which sg backend one graphical example builds against, from SC_EXAMPLE_BACKEND.
#       <out_mode> is `ok` (build it against <out_backend>), `stub` (the chosen backend exists but this example
#       does not support it) or `none` (nothing this example supports was built -- the caller returns).
#
#   sc_add_example_backend_stub(<target> <chosen> SUPPORTS <name>...)
#       A target under the example's own name that prints what was chosen and what the example supports, and
#       exits non-zero. Links nothing graphical, so it builds on a leg where the real target could not.

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
        message(FATAL_ERROR "sc_add_example(${target}): an example target must be named '*-example', the convention for a binary that holds only examples")
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
    sc_nexus_binary(${target} KINDS examples)

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

# SC_EXAMPLE_BACKEND resolved for ONE example, whose own list of supported backends is the other half of it.
#
# Every graphical example reads the knob, not only the one that happens to support both backends: a setting the
# rest ignore is a setting that lies, and `SC_EXAMPLE_BACKEND=vulkan` silently building a dx12 example was the
# shape of that.
function(sc_resolve_example_backend out_backend out_mode)
    cmake_parse_arguments(PARSE_ARGV 2 RB "" "" "SUPPORTS")

    # A named backend that was not built is a configure error for EVERY example, which is what docs/platforms.md
    # already promises: the setting named something this build does not have.
    if(NOT SC_EXAMPLE_BACKEND STREQUAL "auto")
        if(NOT TARGET shaped-graphics-${SC_EXAMPLE_BACKEND})
            message(FATAL_ERROR
                "SC_EXAMPLE_BACKEND=${SC_EXAMPLE_BACKEND} but shaped-graphics-${SC_EXAMPLE_BACKEND} was not built")
        endif()

        # Built, but not one this example can use: a stub under its name rather than a silent omission, so
        # `dev.py example` still resolves it and says why it cannot run.
        if(NOT SC_EXAMPLE_BACKEND IN_LIST RB_SUPPORTS)
            set(${out_backend} "" PARENT_SCOPE)
            set(${out_mode} "stub" PARENT_SCOPE)
            return()
        endif()

        set(${out_backend} "shaped-graphics-${SC_EXAMPLE_BACKEND}" PARENT_SCOPE)
        set(${out_mode} "ok" PARENT_SCOPE)
        return()
    endif()

    # `auto` takes this example's own first choice, which is the order it listed them in.
    foreach(_candidate IN LISTS RB_SUPPORTS)
        if(TARGET shaped-graphics-${_candidate})
            set(${out_backend} "shaped-graphics-${_candidate}" PARENT_SCOPE)
            set(${out_mode} "ok" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    set(${out_backend} "" PARENT_SCOPE)
    set(${out_mode} "none" PARENT_SCOPE)
endfunction()

# The target an example becomes when SC_EXAMPLE_BACKEND named a backend it does not support.
#
# It links nothing graphical on purpose: the real target would link e.g. shaped-graphics-dx12, which does not
# exist on Linux or on the wasm and cross-compile legs, and this has to build everywhere the real one is asked
# for and cannot be.
function(sc_add_example_backend_stub target chosen)
    if(NOT SC_BUILD_EXAMPLES)
        return()
    endif()

    cmake_parse_arguments(PARSE_ARGV 2 ST "" "" "SUPPORTS")
    string(REPLACE ";" ", " _supported "${ST_SUPPORTS}")

    set(_stub "${CMAKE_CURRENT_BINARY_DIR}/${target}-backend-stub.cc")

    # A bracket argument, so the C++ below reaches the file byte for byte.
    # A quoted argument would eat the escapes in its own string literals, and a newline escape arriving at
    # the compiler as a raw newline is a broken source file.
    set(_stub_body [[
// Generated by sc_add_example_backend_stub. Do not edit.
#include <cstdio>

int main()
{
    std::fprintf(stderr,
                 "@target@: SC_EXAMPLE_BACKEND=@chosen@, which this example does not support "
                 "(it supports: @_supported@).\n"
                 "Reconfigure with -DSC_EXAMPLE_BACKEND=auto, or with one it supports.\n");
    return 1;
}
]])
    string(CONFIGURE "${_stub_body}" _stub_text @ONLY)
    file(CONFIGURE OUTPUT "${_stub}" CONTENT "${_stub_text}")

    add_executable(${target} "${_stub}")
    set_target_properties(${target} PROPERTIES FOLDER "examples")
endfunction()
