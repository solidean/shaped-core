# The repo-wide build configuration as a target, so a project consuming shaped-core gets it too.
#
# Our build modules (Simd, FloatingPoint, Sanitizers, Coverage, PGO, Emscripten) used to call add_compile_options and friends.
# Those are directory-scoped: as a subdirectory of another project, shaped-core applied them to its own targets and never to the consumer's.
# Some are ABI, not taste — CC_X64_LEVEL is read by clean-core's headers, and -pthread must agree between compile and link.
# A consumer compiled without them silently disagreed with clean-core's own translation units.
#
# So every repo-wide flag goes through sc_build_options(), which records it twice:
#   - on this directory, as before, so every target here gets it — vendored C libraries that link nothing of ours included;
#   - on the INTERFACE target shaped-core::build-config, which clean-core links PUBLIC, so it reaches everything that links clean-core.
# On our own targets the two meet, and CMake's option de-duplication leaves one copy.
#
# Must be included before any module that calls sc_build_options().

add_library(sc-build-config INTERFACE)
add_library(shaped-core::build-config ALIAS sc-build-config)

# sc_build_options([COMPILE <option>...] [DEFINITIONS <definition>...] [LINK <option>...])
function(sc_build_options)
    cmake_parse_arguments(PARSE_ARGV 0 _o "" "" "COMPILE;DEFINITIONS;LINK")
    if(_o_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "sc_build_options: unexpected argument(s): ${_o_UNPARSED_ARGUMENTS}")
    endif()
    if(_o_COMPILE)
        add_compile_options(${_o_COMPILE})
        target_compile_options(sc-build-config INTERFACE ${_o_COMPILE})
    endif()
    if(_o_DEFINITIONS)
        add_compile_definitions(${_o_DEFINITIONS})
        target_compile_definitions(sc-build-config INTERFACE ${_o_DEFINITIONS})
    endif()
    if(_o_LINK)
        add_link_options(${_o_LINK})
        target_link_options(sc-build-config INTERFACE ${_o_LINK})
    endif()
endfunction()
