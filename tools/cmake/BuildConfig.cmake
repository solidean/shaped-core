# The repo-wide build flags as a target, so a project adding shaped-core with add_subdirectory gets them too.
#
# Our build modules (Simd, FloatingPoint, Sanitizers, Coverage, PGO, Emscripten) used to call add_compile_options and friends.
# Those are directory-scoped: as a subdirectory of another project, shaped-core applied them to its own targets and never to the consumer's.
# Some must agree between compile and link, as -pthread must.
#
# So every repo-wide flag goes through sc_build_options(), which records it twice:
#   - on this directory, as before, so every target here gets it — vendored C libraries that link nothing of ours included;
#   - on the INTERFACE target shaped-core::build-config, which clean-core links PUBLIC, so it reaches everything that links clean-core.
# On our own targets the two meet, and CMake's option de-duplication leaves one copy.
#
# Flags only, and only in the build tree: clean-core links the target as $<BUILD_INTERFACE:...>, so an installed
# clean-core hands its consumers no compile or link option.
# What of the configuration is ABI, the x86-64 level above all, is a #define in clean-core's generated build_config.hh,
# which reaches an installed consumer and an add_subdirectory one alike.
#
# Must be included before any module that calls sc_build_options().

add_library(sc-build-config INTERFACE)
add_library(shaped-core::build-config ALIAS sc-build-config)

# sc_build_options([COMPILE <option>...] [LINK <option>...])
function(sc_build_options)
    cmake_parse_arguments(PARSE_ARGV 0 _o "" "" "COMPILE;LINK")
    if(_o_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "sc_build_options: unexpected argument(s): ${_o_UNPARSED_ARGUMENTS}")
    endif()
    if(_o_COMPILE)
        add_compile_options(${_o_COMPILE})
        target_compile_options(sc-build-config INTERFACE ${_o_COMPILE})
    endif()
    if(_o_LINK)
        add_link_options(${_o_LINK})
        target_link_options(sc-build-config INTERFACE ${_o_LINK})
    endif()
endfunction()
