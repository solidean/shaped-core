# Puts the macOS SDK on every compile and link line, resolved once per configure, so an SDK change is a command-line change.
# Without it, Homebrew clang picks the SDK silently through a per-OS config file, and an OS update hands new-SDK headers to TUs whose PCH was built against the old one.
# docs/requirements.md (macOS: deployment target and SDK) has the whole story.
#
# The SDK is the one the compiler itself would pick now, read from its `-###` output; the last -isysroot there is the one it uses.
# An explicit CMAKE_OSX_SYSROOT wins and this does nothing.
# SC_MACOS_SDK records the result for `dev.py doctor`, which warns once the compiler would pick a different one.

if(APPLE AND NOT IOS AND NOT CMAKE_OSX_SYSROOT)
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" "-###" -x c++ -c /dev/null
        ERROR_VARIABLE _sc_driver_out
        OUTPUT_QUIET
        RESULT_VARIABLE _sc_driver_result)
    string(REGEX MATCHALL "\"-isysroot\" \"[^\"]+\"" _sc_sysroots "${_sc_driver_out}")
    if(_sc_driver_result EQUAL 0 AND _sc_sysroots)
        list(GET _sc_sysroots -1 _sc_sysroot)
        string(REGEX REPLACE "^\"-isysroot\" \"([^\"]+)\"$" "\\1" _sc_sysroot "${_sc_sysroot}")
        set(SC_MACOS_SDK "${_sc_sysroot}" CACHE INTERNAL "the macOS SDK this build directory was configured against")
        sc_build_options(COMPILE "-isysroot${_sc_sysroot}" LINK "-isysroot${_sc_sysroot}")
        message(STATUS "macOS SDK: ${_sc_sysroot}")
    else()
        unset(SC_MACOS_SDK CACHE)
    endif()
endif()
