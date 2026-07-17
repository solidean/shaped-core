# sc_add_shader_package / sc_finalize_shader_packages -- declare a target's shader package.
#
# Included from the root CMakeLists *before* the libs, so any target can call it -- including the test
# binary of a library that shaped-shader-library itself depends on (shaped-graphics-test). That works
# because target_link_libraries resolves item names at generate time, so naming a target defined later
# is legal. The catch is the failure mode: if shaped-shader-library is never defined, CMake silently
# treats the item as a plain library name, slib's include dirs never arrive, and the error surfaces as
# a confusing missing-header inside generated code. sc_finalize_shader_packages turns that into a clear
# message at the end of configure.

set(SC_SHADER_PACKAGE_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/GenerateShaderPackage.cmake"
    CACHE INTERNAL "Generator script backing sc_add_shader_package")

# Fails configure with an actionable message if a package was declared but slib is missing. Call once,
# at the bottom of the root CMakeLists.
function(sc_finalize_shader_packages)
    get_property(_pkg_targets GLOBAL PROPERTY SC_SHADER_PACKAGE_TARGETS)
    if(_pkg_targets AND NOT TARGET shaped-shader-library)
        list(REMOVE_DUPLICATES _pkg_targets)
        list(JOIN _pkg_targets ", " _pkg_list)
        message(FATAL_ERROR
            "sc_add_shader_package was called for [${_pkg_list}], but the shaped-shader-library target was "
            "never defined. Check the add_subdirectory order in the root CMakeLists.")
    endif()
endfunction()
