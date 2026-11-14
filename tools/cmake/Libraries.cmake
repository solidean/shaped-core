# The shaped-core libraries, what each depends on, and which of them this build adds (SC_LIBRARIES).
# Included once from the root CMakeLists before add_subdirectory(extern), since the externals ask whether a subset is built.
#
# The table below is each library's shaped-core dependencies in one place, so the closure of a subset is known before any library is added.
# DEPENDS is every shaped-core library the library links, PUBLIC or PRIVATE, optional ones included.
# TEST_DEPENDS is what its tests, tools and examples link beyond DEPENDS and nexus.
# The CMakeLists files stay the ground truth: sc_check_library_links(), run once every target exists, fails configure
# where a library links a shaped-core library its row does not name.

set(SC_LIBRARIES "" CACHE STRING "The shaped-core libraries to build, with their dependencies; empty builds every library")

get_filename_component(_sc_repo_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# sc_library(<name> <source dir> [DEPENDS <library>...] [TEST_DEPENDS <library>...])
function(sc_library name dir)
    cmake_parse_arguments(PARSE_ARGV 2 _l "" "" "DEPENDS;TEST_DEPENDS")
    if(_l_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "sc_library(${name}): unexpected argument(s): ${_l_UNPARSED_ARGUMENTS}")
    endif()
    set_property(GLOBAL APPEND PROPERTY SC_ALL_LIBRARIES ${name})
    set_property(GLOBAL PROPERTY SC_LIBRARY_DIR_${name} "${_sc_repo_root}/${dir}")
    set_property(GLOBAL PROPERTY SC_LIBRARY_DEPENDS_${name} "${_l_DEPENDS}")
    set_property(GLOBAL PROPERTY SC_LIBRARY_TEST_DEPENDS_${name} "${_l_TEST_DEPENDS}")
endfunction()

sc_library(clean-core libs/base/clean-core)
sc_library(typed-geometry libs/base/typed-geometry DEPENDS clean-core)
sc_library(clean-net libs/base/clean-net DEPENDS clean-core)
sc_library(clean-simd libs/base/clean-simd DEPENDS clean-core)
sc_library(babel-data libs/data/babel-data DEPENDS clean-core)
sc_library(babel-serializer libs/data/babel-serializer DEPENDS babel-data typed-geometry)
sc_library(versioned-document libs/data/versioned-document DEPENDS clean-core)
sc_library(versioned-document-file libs/data/versioned-document-file DEPENDS versioned-document babel-serializer)
sc_library(blob-cache libs/data/blob-cache DEPENDS clean-core babel-serializer)
sc_library(shaped-graphics libs/graphics/shaped-graphics
    DEPENDS clean-core typed-geometry blob-cache
    TEST_DEPENDS shaped-shader-library shaped-graphics-language)
sc_library(shaped-rendering libs/graphics/shaped-rendering
    DEPENDS shaped-graphics shaped-shader-library babel-serializer
    TEST_DEPENDS shaped-graphics-language)
sc_library(shaped-xr libs/graphics/shaped-xr DEPENDS shaped-rendering)
sc_library(shaped-viewer libs/graphics/shaped-viewer DEPENDS shaped-rendering babel-serializer clean-net)
sc_library(shaped-graphics-language libs/graphics/shaped-graphics-language
    DEPENDS clean-core
    TEST_DEPENDS babel-data babel-serializer)
sc_library(shader-analysis libs/graphics/shader-analysis DEPENDS clean-core babel-data)
sc_library(shaped-shader-compiler-dxc libs/graphics/shaped-shader-compiler-dxc DEPENDS shaped-graphics)
sc_library(shaped-shader-compiler-msl libs/graphics/shaped-shader-compiler-msl DEPENDS shaped-graphics)
sc_library(shaped-shader-library libs/graphics/shaped-shader-library
    DEPENDS shaped-graphics shaped-graphics-language blob-cache shaped-shader-compiler-dxc shaped-shader-compiler-msl)
sc_library(nexus libs/base/nexus DEPENDS clean-core babel-data)

# sc_library_closure(<out var> [TESTS] <library>...): the libraries and every library they depend on.
# TESTS adds what their tests, tools and examples link: nexus and each TEST_DEPENDS, whose own closure in turn.
function(sc_library_closure out)
    cmake_parse_arguments(PARSE_ARGV 1 _c "TESTS" "" "")
    set(_todo ${_c_UNPARSED_ARGUMENTS})
    if(_c_TESTS)
        list(APPEND _todo nexus)
    endif()
    get_property(_all GLOBAL PROPERTY SC_ALL_LIBRARIES)
    set(_done "")
    while(_todo)
        list(POP_FRONT _todo _lib)
        if(_lib IN_LIST _done)
            continue()
        endif()
        if(NOT _lib IN_LIST _all)
            message(FATAL_ERROR "'${_lib}' is no shaped-core library; the libraries are: ${_all}")
        endif()
        list(APPEND _done ${_lib})
        get_property(_deps GLOBAL PROPERTY SC_LIBRARY_DEPENDS_${_lib})
        list(APPEND _todo ${_deps})
        if(_c_TESTS)
            get_property(_test_deps GLOBAL PROPERTY SC_LIBRARY_TEST_DEPENDS_${_lib})
            list(APPEND _todo ${_test_deps})
        endif()
    endwhile()
    set(${out} ${_done} PARENT_SCOPE)
endfunction()

# Whether this build adds a library's tests, tools or examples, which link nexus and their TEST_DEPENDS.
if(SC_BUILD_TESTS OR SC_BUILD_TOOLS OR SC_BUILD_EXAMPLES)
    set(_sc_closure_tests TESTS)
else()
    set(_sc_closure_tests "")
endif()

# SC_BUILT_LIBRARIES: what this build adds, a variable of the root scope and a global property for the end-of-configure check.
# SC_LIBRARIES itself stays what was asked for.
# Empty SC_LIBRARIES is every library, exactly as before the switch existed.
if(SC_LIBRARIES)
    sc_library_closure(SC_BUILT_LIBRARIES ${_sc_closure_tests} ${SC_LIBRARIES})
    set(SC_LIBRARY_SUBSET ON)
    message(STATUS "SC_LIBRARIES: building ${SC_BUILT_LIBRARIES}")
else()
    get_property(SC_BUILT_LIBRARIES GLOBAL PROPERTY SC_ALL_LIBRARIES)
    set(SC_LIBRARY_SUBSET OFF)
endif()
set_property(GLOBAL PROPERTY SC_BUILT_LIBRARIES ${SC_BUILT_LIBRARIES})

# sc_add_library(<name>): add the library's directory, if this build adds it.
# A platform or environment gate (DXC fetched, an Apple target) still wraps the call; the closure says only what may be added.
macro(sc_add_library name)
    if("${name}" IN_LIST SC_BUILT_LIBRARIES)
        get_property(_sc_dir GLOBAL PROPERTY SC_LIBRARY_DIR_${name})
        add_subdirectory("${_sc_dir}")
    endif()
endmacro()

# The shaped-core libraries named in a list of link items, generator expressions stripped.
function(_sc_linked_libraries out)
    get_property(_all GLOBAL PROPERTY SC_ALL_LIBRARIES)
    set(_found "")
    foreach(_item IN LISTS ARGN)
        string(REGEX REPLACE "\\$<[A-Za-z_]+:" ";" _item "${_item}")
        string(REGEX REPLACE "[>,]" ";" _item "${_item}")
        foreach(_token IN LISTS _item)
            if(_token IN_LIST _all)
                list(APPEND _found ${_token})
            endif()
        endforeach()
    endforeach()
    set(${out} ${_found} PARENT_SCOPE)
endfunction()

function(_sc_directory_targets out dir)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_sub IN LISTS _subdirs)
        _sc_directory_targets(_sub_targets "${_sub}")
        list(APPEND _targets ${_sub_targets})
    endforeach()
    set(${out} ${_targets} PARENT_SCOPE)
endfunction()

# sc_check_library_links(): fail configure where the table above and the libraries' own CMakeLists disagree.
# A library's own target may link only the closure of its DEPENDS; every other target of its directory tree — tests,
# tools, examples, backends — also its TEST_DEPENDS and nexus.
# Every shaped-core library linked must also be one this build adds, which is what makes a subset configure or fail here
# rather than link against a library name that is no target.
function(sc_check_library_links)
    get_property(_built GLOBAL PROPERTY SC_BUILT_LIBRARIES)
    set(_errors "")
    foreach(_lib IN LISTS _built)
        if(NOT TARGET ${_lib})
            continue() # gated off on this platform, or in this environment
        endif()
        get_property(_dir GLOBAL PROPERTY SC_LIBRARY_DIR_${_lib})
        get_property(_deps GLOBAL PROPERTY SC_LIBRARY_DEPENDS_${_lib})
        get_property(_test_deps GLOBAL PROPERTY SC_LIBRARY_TEST_DEPENDS_${_lib})
        sc_library_closure(_strict ${_deps})
        sc_library_closure(_lenient TESTS ${_lib} ${_deps} ${_test_deps})
        _sc_directory_targets(_targets "${_dir}")
        foreach(_target IN LISTS _targets)
            get_target_property(_type ${_target} TYPE)
            set(_links "")
            if(NOT _type STREQUAL "INTERFACE_LIBRARY")
                get_target_property(_links ${_target} LINK_LIBRARIES)
            endif()
            get_target_property(_interface ${_target} INTERFACE_LINK_LIBRARIES)
            _sc_linked_libraries(_linked ${_links} ${_interface})
            if(_target STREQUAL _lib)
                set(_allowed ${_strict})
                set(_row "DEPENDS")
            else()
                set(_allowed ${_lenient})
                set(_row "DEPENDS or TEST_DEPENDS")
            endif()
            foreach(_m IN LISTS _linked)
                if(_m STREQUAL _lib)
                    continue()
                endif()
                if(NOT _m IN_LIST _allowed)
                    list(APPEND _errors "  ${_target} links ${_m}, which its row in tools/cmake/Libraries.cmake does not name in ${_row}")
                elseif(NOT _m IN_LIST _built)
                    list(APPEND _errors "  ${_target} links ${_m}, which this build does not add")
                endif()
            endforeach()
        endforeach()
    endforeach()
    if(_errors)
        list(REMOVE_DUPLICATES _errors)
        list(JOIN _errors "\n" _errors)
        message(FATAL_ERROR "The library table and the libraries' CMakeLists disagree:\n${_errors}")
    endif()
endfunction()
