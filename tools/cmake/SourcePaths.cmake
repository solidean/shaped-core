# sc_target_source_paths — a source-tree directory handed to code as a path relative to the top-level source directory.
#
#     sc_target_source_paths(<target> <PRIVATE|PUBLIC|INTERFACE>
#         <MACRO> <path>
#         [<MACRO> <path> ...])
#
# Each <path> is relative to the calling CMakeLists, or absolute, and must lie inside CMAKE_SOURCE_DIR.
# Each <MACRO> becomes a string literal of that path relative to CMAKE_SOURCE_DIR.
# The code joins it onto nx::source_root() at run time (libs/base/nexus/src/nexus/source_root.hh).
#
# Never an absolute path: a binary carrying one depends on where its checkout sits.
# No compile is then shared between two checkouts, and a build directory reads another checkout's data without a word.
# `dev.py check`'s `abspaths` gate refuses one in any compile definition or generated source.
function(sc_target_source_paths target scope)
    if(NOT scope MATCHES "^(PRIVATE|PUBLIC|INTERFACE)$")
        message(FATAL_ERROR "sc_target_source_paths(${target}): the scope must be PRIVATE, PUBLIC or INTERFACE, not '${scope}'")
    endif()
    set(_pairs ${ARGN})
    list(LENGTH _pairs _count)
    math(EXPR _odd "${_count} % 2")
    if(_count EQUAL 0 OR _odd)
        message(FATAL_ERROR "sc_target_source_paths(${target}): expected <MACRO> <path> pairs, got '${ARGN}'")
    endif()

    set(_definitions "")
    math(EXPR _last "${_count} - 1")
    foreach(_i RANGE 0 ${_last} 2)
        math(EXPR _j "${_i} + 1")
        list(GET _pairs ${_i} _macro)
        list(GET _pairs ${_j} _path)
        cmake_path(ABSOLUTE_PATH _path BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE OUTPUT_VARIABLE _absolute)
        cmake_path(IS_PREFIX CMAKE_SOURCE_DIR "${_absolute}" NORMALIZE _inside)
        if(NOT _inside)
            message(FATAL_ERROR "sc_target_source_paths(${target}): ${_macro} names '${_absolute}', "
                                "which is outside the source tree '${CMAKE_SOURCE_DIR}'")
        endif()
        # empty for the source root itself, which nx::source_path("") answers with the root
        file(RELATIVE_PATH _relative "${CMAKE_SOURCE_DIR}" "${_absolute}")
        list(APPEND _definitions "${_macro}=\"${_relative}\"")
    endforeach()
    target_compile_definitions(${target} ${scope} ${_definitions})
endfunction()
