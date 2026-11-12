# install() rules and the CMake package `shaped-core`, for a consumer that takes shaped-core prebuilt rather than as a subdirectory.
# Included once from the root CMakeLists after every library is added; nothing happens unless SC_INSTALL is ON.
#
# What `cmake --install` lays out under its prefix:
#   lib/                              every built library's static archive, and those of the vendored externals they link
#   include/                          each library's public headers at the paths consumers include, and clean-core/build_config.hh
#   lib/cmake/shaped-core/            shaped-coreConfig.cmake, its version file and the exported targets, shaped-core::<library>
#   share/shaped-core/                LICENSE, and the license of each vendored external installed beside it
#
# An installed clean-core carries no compile definition and no compile or link option of the build that made it.
# What is ABI is in build_config.hh, which its headers include; the build's code-generation flags stay with the build.
# nexus, the tests, tools and examples are never installed.
#
# A library is installable when everything it links is: another installed library, a vendored external of extern/ whose
# headers no consumer needs, a system library named plainly, or an imported target of a package shaped-coreConfig.cmake
# finds again (Threads, and the externals extern/CMakeLists.txt found by find_package).
# sc_install_libraries() fails configure naming what is not, rather than writing a package that cannot link.

option(SC_INSTALL "Generate install() rules and the shaped-core CMake package for the built libraries" OFF)

# shaped-core has no release version of its own yet, so whoever packages it says which this is, a package recipe its own.
set(SC_PACKAGE_VERSION "0.0.0" CACHE STRING "The version shaped-coreConfigVersion.cmake reports")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

get_filename_component(_sc_install_repo_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# The plain link items of a target, generator expressions resolved the way an installed consumer sees them:
# $<BUILD_INTERFACE:...> dropped, $<LINK_ONLY:...> and $<INSTALL_INTERFACE:...> unwrapped.
function(_sc_install_link_items out target)
    get_target_property(_type ${target} TYPE)
    set(_items "")
    if(NOT _type STREQUAL "INTERFACE_LIBRARY")
        get_target_property(_private ${target} LINK_LIBRARIES)
        if(_private)
            list(APPEND _items ${_private})
        endif()
    endif()
    get_target_property(_interface ${target} INTERFACE_LINK_LIBRARIES)
    if(_interface)
        list(APPEND _items ${_interface})
    endif()
    set(_plain "")
    foreach(_item IN LISTS _items)
        if(_item MATCHES "^\\$<BUILD_INTERFACE:")
            continue()
        endif()
        string(REGEX REPLACE "^\\$<(LINK_ONLY|INSTALL_INTERFACE):(.*)>$" "\\2" _item "${_item}")
        if(_item MATCHES "^\\$<")
            message(FATAL_ERROR "SC_INSTALL: ${target} links '${_item}', a generator expression the install rules do not read yet")
        endif()
        list(APPEND _plain "${_item}")
    endforeach()
    set(${out} ${_plain} PARENT_SCOPE)
endfunction()

# Fails where a target's interface names a path of the source or build tree outside $<BUILD_INTERFACE:...>.
function(_sc_install_check_paths target)
    foreach(_property INTERFACE_INCLUDE_DIRECTORIES INTERFACE_COMPILE_DEFINITIONS INTERFACE_COMPILE_OPTIONS INTERFACE_LINK_OPTIONS)
        get_target_property(_values ${target} ${_property})
        if(NOT _values)
            continue()
        endif()
        foreach(_value IN LISTS _values)
            if(_value MATCHES "^\\$<BUILD_INTERFACE:")
                continue()
            endif()
            if(_value MATCHES "^${_sc_install_repo_root}/" OR _value MATCHES "^${CMAKE_BINARY_DIR}/")
                message(FATAL_ERROR "SC_INSTALL: ${target}'s ${_property} names '${_value}' outside $<BUILD_INTERFACE:...>, "
                                    "which an installed package cannot carry")
            endif()
        endforeach()
    endforeach()
endfunction()

# sc_install_libraries(): the install rules and the package, for every library this build adds but nexus.
function(sc_install_libraries)
    if(NOT SC_INSTALL)
        return()
    endif()
    get_property(_all GLOBAL PROPERTY SC_ALL_LIBRARIES)
    get_property(_built GLOBAL PROPERTY SC_BUILT_LIBRARIES)
    get_property(_packages GLOBAL PROPERTY SC_EXTERNAL_PACKAGES)
    get_property(_package_targets GLOBAL PROPERTY SC_EXTERNAL_PACKAGE_TARGETS)

    set(_libraries "")
    foreach(_lib IN LISTS _built)
        if(NOT _lib STREQUAL "nexus" AND TARGET ${_lib})
            list(APPEND _libraries ${_lib})
        endif()
    endforeach()

    # Walk what the libraries link: the vendored externals join the export, the imported targets need their packages.
    set(_installed ${_libraries})
    set(_todo ${_libraries})
    set(_errors "")
    while(_todo)
        list(POP_FRONT _todo _target)
        _sc_install_check_paths(${_target})
        _sc_install_link_items(_items ${_target})
        foreach(_item IN LISTS _items)
            if(NOT TARGET ${_item})
                if(_item MATCHES "::" AND NOT _item STREQUAL "Threads::Threads")
                    # An imported target of another directory's find_package, which no find_dependency here would define.
                    list(APPEND _errors "  ${_target} links ${_item}, an imported target shaped-coreConfig.cmake does not find")
                endif()
                continue() # a system library or a linker flag, written as it is
            endif()
            get_target_property(_aliased ${_item} ALIASED_TARGET)
            if(_aliased)
                set(_item ${_aliased})
            endif()
            if(_item IN_LIST _installed)
                continue()
            endif()
            get_target_property(_imported ${_item} IMPORTED)
            if(_imported)
                if(_item STREQUAL "Threads::Threads" OR _item IN_LIST _package_targets)
                    continue()
                endif()
                list(APPEND _errors "  ${_target} links the imported ${_item}, which no package shaped-coreConfig.cmake finds provides")
                continue()
            endif()
            if(_item IN_LIST _all)
                list(APPEND _errors "  ${_target} links ${_item}, a library this build does not add")
                continue()
            endif()
            get_target_property(_source_dir ${_item} SOURCE_DIR)
            if(NOT _source_dir MATCHES "^${_sc_install_repo_root}/extern/")
                list(APPEND _errors "  ${_target} links ${_item} (${_source_dir}), which is neither a library nor a vendored external")
                continue()
            endif()
            list(APPEND _installed ${_item})
            list(APPEND _todo ${_item})
        endforeach()
    endwhile()
    if(_errors)
        list(REMOVE_DUPLICATES _errors)
        list(JOIN _errors "\n" _errors)
        message(FATAL_ERROR "SC_INSTALL: these libraries are not installable yet:\n${_errors}")
    endif()

    foreach(_target IN LISTS _installed)
        if(_target IN_LIST _libraries)
            install(TARGETS ${_target} EXPORT shaped-core-targets
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
                LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
                FILE_SET public_headers DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
            # Every header under the file set's base directory, too: the file sets do not list every header a listed one
            # includes (clean-core's error/exception_base.hh, for one), and a consumer needs them all.
            get_target_property(_header_dirs ${_target} HEADER_DIRS_public_headers)
            foreach(_header_dir IN LISTS _header_dirs)
                install(DIRECTORY "${_header_dir}/" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} FILES_MATCHING PATTERN "*.hh")
            endforeach()
        else()
            # A vendored external: its archive only, under a shaped-core:: name so it cannot meet a consumer's own copy's target.
            install(TARGETS ${_target} EXPORT shaped-core-targets
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
                LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
            get_target_property(_source_dir ${_target} SOURCE_DIR)
            file(GLOB _licenses "${_source_dir}/LICENSE*" "${_source_dir}/COPYING*")
            if(_licenses)
                install(FILES ${_licenses} DESTINATION ${CMAKE_INSTALL_DATADIR}/shaped-core/licenses/${_target})
            endif()
        endif()
    endforeach()

    # clean-core's generated configuration header, the one for the configuration being installed.
    get_target_property(_cc_binary_dir clean-core BINARY_DIR)
    install(FILES "${_cc_binary_dir}/generated/$<CONFIG>/clean-core/build_config.hh"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/clean-core)

    install(FILES "${_sc_install_repo_root}/LICENSE" DESTINATION ${CMAKE_INSTALL_DATADIR}/shaped-core)

    set(_config_dir ${CMAKE_INSTALL_LIBDIR}/cmake/shaped-core)
    install(EXPORT shaped-core-targets NAMESPACE shaped-core:: DESTINATION ${_config_dir} FILE shaped-core-targets.cmake)

    # The packages shaped-coreConfig.cmake finds again before it defines the targets that link theirs.
    set(SC_INSTALL_FIND_DEPENDENCIES "find_dependency(Threads)\n")
    if(_packages)
        list(REMOVE_DUPLICATES _packages)
        foreach(_package IN LISTS _packages)
            string(APPEND SC_INSTALL_FIND_DEPENDENCIES "find_dependency(${_package})\n")
        endforeach()
    endif()
    list(JOIN _libraries ", " SC_INSTALL_LIBRARIES)
    configure_package_config_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/shaped-coreConfig.cmake.in"
        "${CMAKE_BINARY_DIR}/shaped-coreConfig.cmake"
        INSTALL_DESTINATION ${_config_dir}
        NO_SET_AND_CHECK_MACRO NO_CHECK_REQUIRED_COMPONENTS_MACRO)
    write_basic_package_version_file("${CMAKE_BINARY_DIR}/shaped-coreConfigVersion.cmake"
        VERSION ${SC_PACKAGE_VERSION} COMPATIBILITY SameMinorVersion)
    install(FILES "${CMAKE_BINARY_DIR}/shaped-coreConfig.cmake" "${CMAKE_BINARY_DIR}/shaped-coreConfigVersion.cmake"
            DESTINATION ${_config_dir})
endfunction()
