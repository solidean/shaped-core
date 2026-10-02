# sc_add_shader_package / sc_finalize_shader_packages -- declare a target's shader package.
#
# Included from the root CMakeLists *before* the libs, so any target can call it -- including the test
# binary of a library that shaped-shader-library itself depends on (shaped-graphics-test).
# That works because target_link_libraries resolves item names at generate time, so naming a target defined later is legal.
# The catch is the failure mode: if shaped-shader-library is never defined, CMake silently treats the item as a plain
# library name, slib's include dirs never arrive, and the error surfaces as a confusing missing-header inside generated code.
# sc_finalize_shader_packages turns that into a clear message at the end of configure.
#
#   sc_add_shader_package(
#       TARGET     my-renderer
#       NAME       my_shaders                  # package id; also the header name and the mount point
#       NAMESPACE  my::shaders                 # where the generated symbols live
#       SOURCE_DIR shaders                     # relative to the calling CMakeLists
#       LANGUAGE   hlsl
#       SHADERS
#           vignette.hlsl:compute:main         # path:stage:entry_point
#           blit.hlsl:vertex:main_vs
#           blit.hlsl:fragment:main_ps
#   )
#
#   #include <my_shaders.hh>
#   auto cs = my::shaders::vignette.compute.main->acquire(ctx);
#
# Four more kinds of entry generate C++ from what the binding pass reads, rather than an entry point:
#
#           frame_bindings.hlsli:binding:frame_bindings    # an annotated namespace -> one group struct
#           mesh.hlsl:vertex_input:vs_input               # an annotated struct -> a mirror + vertex_layout_of
#           mesh.hlsl:payload:trace_payload               # an annotated struct -> a mirror + max_payload_size
#           shade.hlsl:constants:gConstants               # a push_constants block -> a mirror, with HLSL's padding
#
# The third field is what the shader named: a namespace for `binding`, a struct for `vertex_input` and
# `payload`, and the ConstantBuffer's own name for `constants`.
#
# Each generates from the NAMED FILE and never from its includes, so an .hlsli that declares a group is
# registered on its own, in whichever package owns it -- otherwise every shader including it would generate
# the same struct again.
#
# A group struct is data rather than an API: it satisfies sg::declared_binding_group, and the verbs are the
# scopes' -- ctx.cached.acquire_binding_group_layout<G>(), ctx.transient.create_binding_group(cmd, layout, G{...}) and
# scope.bind<G>(group).
#
# A shader in a subdirectory folds the directory into the identifier: post/vignette.hlsl reaches C++ as
# post_vignette, not vignette.
#
# Stages are spelled exactly as sg::shader_stage (compute / vertex / fragment / raygen / ...), so the
# generator emits the enumerator instead of a string for C++ to parse back.
#
# LANGUAGE is hlsl (the default), wgsl or sgl.
# The four generating kinds above read HLSL, so a WGSL package that names one is a generator error.
#
# An SGL package spells its stages as SGL does -- `cube.sgl:vertex:main_vs`, `cube.sgl:pixel:main_ps` -- and
# `pixel` is sg's fragment stage.
# It has three generating kinds of its own, and one entry that asks for a whole file:
#
#           cube.sgl:*                                   # every entry point and every declaration below
#           cube.sgl:binding:frame                       # a `binding` block -> a group struct
#           cube.sgl:vertex_input:cube_vertex            # a `@vertex struct` -> its mirror and vertex layout
#           cube.sgl:render_target:target                # a `@pixel struct` -> its named color targets
#
# The third field is the SGL name exactly as the shader spells it, and one the file does not declare is a build error.
# `*` generates nothing for what the file `use`s: an imported module is described by its own package entry.
#
# An SGL package's SOURCE_DIR is a module directory, and MODULE_DIRS names more, relative to the calling CMakeLists:
# a file's `use view` finds module `view` among the `.sgl` files directly in any of them.
# A module another package exports is reached by naming that package's directory here too.
#
#           module:view                                  # every binding, struct, @vertex and @pixel struct of `view`
#
# A module's types land in <NAMESPACE>::view, in their own header <sgl_modules/view.hh>, which also declares the alias
# `sgl_modules::view` that generated code names them through.
# That header's directory is PUBLIC on TARGET, so linking the package's target is what makes a module reachable.
# A file whose entry point lists a module binding no package exports fails on that missing header.
#
# Every SGL entry is read by the SGL compiler itself, `sgl describe`, because it is the one parser of the language.
# An entry point listed on its own is too, since its asset keys its compiles on the options it reaches.
# So an SGL package needs a runnable `sgl` while it builds:
# the tree's own `sgl` target in a native build, or SC_SGL_TOOL, which also serves a cross build and an
# add_subdirectory consumer that builds no tools.

set(SC_SHADER_PACKAGE_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/generate_shader_package.py"
    CACHE INTERNAL "Generator script backing sc_add_shader_package")
set(SC_SHADER_PACKAGE_GRAMMAR "${CMAKE_CURRENT_LIST_DIR}/binding_grammar.py"
    CACHE INTERNAL "The binding grammar the generator imports")
set(SC_SHADER_PACKAGE_SGL "${CMAKE_CURRENT_LIST_DIR}/sgl_description.py"
    CACHE INTERNAL "How the generator reads an SGL package, through the compiler")
set(SC_SHADER_PACKAGE_SGL_HOST "${CMAKE_CURRENT_LIST_DIR}/sgl_host_code.py"
    CACHE INTERNAL "The C++ an SGL package's typed entries become")

set(SC_SGL_TOOL "" CACHE FILEPATH
    "A runnable `sgl`, for an SGL shader package's `*` and typed entries where the tree's own cannot run or is not built")

function(sc_add_shader_package)
    cmake_parse_arguments(PKG "" "TARGET;NAME;NAMESPACE;SOURCE_DIR;LANGUAGE" "SHADERS;MODULE_DIRS" ${ARGN})

    foreach(_required TARGET NAME NAMESPACE SOURCE_DIR SHADERS)
        if(NOT PKG_${_required})
            message(FATAL_ERROR "sc_add_shader_package: ${_required} is required")
        endif()
    endforeach()
    if(PKG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "sc_add_shader_package: unexpected arguments: ${PKG_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT PKG_LANGUAGE)
        set(PKG_LANGUAGE hlsl)
    endif()
    if(NOT TARGET ${PKG_TARGET})
        message(FATAL_ERROR "sc_add_shader_package: '${PKG_TARGET}' is not a target")
    endif()

    # add_custom_command(OUTPUT) attaches to the directory scope it is called from, and a function does not create one.
    # So this must be called where the target is defined, or the outputs and the target that consumes them
    # land in different scopes and nothing builds them.
    get_target_property(_target_dir ${PKG_TARGET} SOURCE_DIR)
    if(NOT "${_target_dir}" STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}")
        message(FATAL_ERROR
            "sc_add_shader_package(${PKG_TARGET}): must be called from the CMakeLists that defines the "
            "target (${_target_dir}), not from ${CMAKE_CURRENT_SOURCE_DIR}")
    endif()

    # normalized as MODULE_DIRS are, since slib compares the two to add a directory listed by two packages once
    cmake_path(ABSOLUTE_PATH PKG_SOURCE_DIR BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE OUTPUT_VARIABLE _source_dir)
    if(NOT IS_DIRECTORY "${_source_dir}")
        message(FATAL_ERROR "sc_add_shader_package(${PKG_NAME}): SOURCE_DIR '${_source_dir}' is not a directory")
    endif()
    set(_module_dirs "")
    foreach(_dir IN LISTS PKG_MODULE_DIRS)
        cmake_path(ABSOLUTE_PATH _dir BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE OUTPUT_VARIABLE _absolute)
        if(NOT IS_DIRECTORY "${_absolute}")
            message(FATAL_ERROR "sc_add_shader_package(${PKG_NAME}): MODULE_DIRS '${_dir}' is not a directory")
        endif()
        list(APPEND _module_dirs "${_absolute}")
    endforeach()
    if(_module_dirs AND NOT PKG_LANGUAGE STREQUAL "sgl")
        message(FATAL_ERROR "sc_add_shader_package(${PKG_NAME}): MODULE_DIRS is for an SGL package")
    endif()

    # Per target, not just per name: two targets in one directory sharing a NAME would collide.
    set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/shader-packages/${PKG_TARGET}")
    set(_manifest "${_gen_dir}/${PKG_NAME}.manifest")
    set(_gen_hh "${_gen_dir}/${PKG_NAME}.hh")
    set(_gen_cc "${_gen_dir}/${PKG_NAME}.cc")

    # The manifest carries the whole declaration, so the custom command below passes only two scalar paths.
    # A CMake list through -D would split on its own ';' before the script saw it.
    set(_manifest_text "# Generated by sc_add_shader_package. Consumed by generate_shader_package.py.\n")
    string(APPEND _manifest_text "NAME=${PKG_NAME}\n")
    string(APPEND _manifest_text "NAMESPACE=${PKG_NAMESPACE}\n")
    string(APPEND _manifest_text "SOURCE_DIR=${_source_dir}\n")
    string(APPEND _manifest_text "LANGUAGE=${PKG_LANGUAGE}\n")
    foreach(_dir IN LISTS _module_dirs)
        string(APPEND _manifest_text "MODULE_DIR=${_dir}\n")
    endforeach()
    foreach(_shader IN LISTS PKG_SHADERS)
        string(APPEND _manifest_text "SHADER=${_shader}\n")
    endforeach()

    # file(CONFIGURE), not file(WRITE): copy-if-different, so a configure that changes nothing does not bump the
    # manifest's mtime and retrigger codegen + compile + link on every preset `dev.py check` builds.
    file(CONFIGURE OUTPUT "${_manifest}" CONTENT "${_manifest_text}" @ONLY)

    # An SGL package runs `sgl describe` over its entries while it builds.
    set(_sgl_args "")
    set(_sgl_depends "")
    # `module:view` writes modules/sgl_modules/view.hh and .cc beside the package's own header
    set(_module_headers "")
    set(_module_sources "")
    if(PKG_LANGUAGE STREQUAL "sgl")
        foreach(_shader IN LISTS PKG_SHADERS)
            string(REPLACE ":" ";" _parts "${_shader}")
            list(GET _parts 0 _first)
            list(GET _parts 1 _kind)
            if(_first STREQUAL "module")
                # One exporter per module: each writes <sgl_modules/NAME.hh> into its own include directory, and a
                # target linking two would silently take whichever comes first on its include path.
                get_property(_exporter GLOBAL PROPERTY "SC_SGL_EXPORTED_MODULE_${_kind}")
                if(_exporter AND NOT _exporter STREQUAL "${PKG_TARGET}/${PKG_NAME}")
                    message(FATAL_ERROR
                        "sc_add_shader_package(${PKG_NAME}): module:${_kind} is exported by ${_exporter} already; "
                        "a module has one exporting package, which every other one reaches through its MODULE_DIRS")
                endif()
                set_property(GLOBAL PROPERTY "SC_SGL_EXPORTED_MODULE_${_kind}" "${PKG_TARGET}/${PKG_NAME}")
                list(APPEND _module_headers "${_gen_dir}/modules/sgl_modules/${_kind}.hh")
                list(APPEND _module_sources "${_gen_dir}/modules/sgl_modules/${_kind}.cc")
            endif()
        endforeach()

        if(SC_SGL_TOOL)
            set(_sgl_args --sgl-tool "${SC_SGL_TOOL}")
            set(_sgl_depends "${SC_SGL_TOOL}")
        elseif(CMAKE_CROSSCOMPILING)
            # The tree's `sgl` is built for the target here, so the build machine could not run it.
            message(FATAL_ERROR
                "sc_add_shader_package(${PKG_NAME}): an SGL package needs a runnable `sgl`, and this "
                "build compiles for another machine. Set SC_SGL_TOOL to a natively built one.")
        elseif(NOT SC_BUILD_TOOLS)
            message(FATAL_ERROR
                "sc_add_shader_package(${PKG_NAME}): an SGL package needs `sgl`, which "
                "SC_BUILD_TOOLS=OFF does not build. Set SC_SGL_TOOL to one.")
        else()
            # A target defined after this call, which is legal: the generator expression and the edge resolve at generate time.
            set(_sgl_args --sgl-tool "$<TARGET_FILE:sgl>")
            set(_sgl_depends sgl)
        endif()
    endif()

    # DEPENDS covers the declared shaders; DEPFILE covers the #include closure the generator discovers.
    set(_shader_files "")
    foreach(_shader IN LISTS PKG_SHADERS)
        string(REPLACE ":" ";" _parts "${_shader}")
        list(GET _parts 0 _path)
        if(NOT _path STREQUAL "module")
            list(APPEND _shader_files "${_source_dir}/${_path}")
        endif()
    endforeach()
    # Every module file is embedded, and one added to a module directory changes neither the manifest nor the depfile,
    # so the directories are globbed with CONFIGURE_DEPENDS: a file appearing re-runs configure, then the generator.
    if(PKG_LANGUAGE STREQUAL "sgl")
        foreach(_dir IN ITEMS "${_source_dir}" ${_module_dirs})
            file(GLOB _found CONFIGURE_DEPENDS "${_dir}/*.sgl")
            list(APPEND _shader_files ${_found})
        endforeach()
    endif()
    list(REMOVE_DUPLICATES _shader_files)

    add_custom_command(
        OUTPUT "${_gen_hh}" "${_gen_cc}" ${_module_headers} ${_module_sources}
        COMMAND uv run "${SC_SHADER_PACKAGE_SCRIPT}" --manifest "${_manifest}" --out-dir "${_gen_dir}" ${_sgl_args}
        DEPENDS "${_manifest}" "${SC_SHADER_PACKAGE_SCRIPT}" "${SC_SHADER_PACKAGE_GRAMMAR}" "${SC_SHADER_PACKAGE_SGL}"
                "${SC_SHADER_PACKAGE_SGL_HOST}" ${_shader_files} ${_sgl_depends}
        DEPFILE "${_gen_dir}/${PKG_NAME}.d"
        COMMENT "[slib] shader package ${PKG_NAME} (${PKG_TARGET})"
        VERBATIM
    )

    # A target of its own over the same outputs, so something other than the package target can be ordered
    # behind the generator.
    # The sibling test includes the generated header but consumes neither generated SOURCE, and an include
    # directory carries no dependency edge -- so without this the test's TU and the generator are unordered.
    # Named per PACKAGE, not per target: a target may declare more than one, and two add_custom_target calls
    # under one name is a configure error.
    add_custom_target(${PKG_TARGET}-${PKG_NAME}-shader-package DEPENDS "${_gen_hh}" "${_gen_cc}" ${_module_headers} ${_module_sources})

    # Plain PRIVATE sources, never a FILE_SET: the generated header lives in the binary dir, and a FILE_SET
    # hard-errors on anything outside its BASE_DIRS.
    # It is per-target private API anyway -- to publish a shader, re-expose it from your own public header
    # (see slib's coding guidelines).
    target_sources(${PKG_TARGET} PRIVATE "${_gen_hh}" "${_gen_cc}" ${_module_headers} ${_module_sources})
    target_include_directories(${PKG_TARGET} PRIVATE "${_gen_dir}")
    # A module's header is the one generated header another target may include, which linking this target gives it.
    if(PKG_LANGUAGE STREQUAL "sgl")
        target_include_directories(${PKG_TARGET} PUBLIC "$<BUILD_INTERFACE:${_gen_dir}/modules>")
    endif()

    if(NOT PKG_TARGET STREQUAL "shaped-shader-library")
        target_link_libraries(${PKG_TARGET} PRIVATE shaped-shader-library)
    endif()

    # Stage the DXC runtime DLLs next to any executable that declares a package.
    # slib's dxc adapter links ssc-dxc, which gives the binary a load-time import of dxcompiler.dll --
    # without it beside the exe the process does not start at all.
    # dxil.dll is not the imported target, so $<TARGET_RUNTIME_DLLS> alone would miss it
    # (same reasoning as shaped-shader-compiler-dxc/CMakeLists.txt).
    #
    # Once per target, not once per package: the staging step is named after the target, and a second package would declare it again.
    if(SC_HAS_DXC_COMPILER AND DXC_RUNTIME_DLLS)
        get_target_property(_type ${PKG_TARGET} TYPE)
        get_target_property(_staged ${PKG_TARGET} SC_SHADER_PACKAGE_DXC_STAGED)
        if(_type STREQUAL "EXECUTABLE" AND NOT _staged)
            sc_stage_runtime_dlls(${PKG_TARGET} TAG ${PKG_TARGET}-dxc DLLS ${DXC_RUNTIME_DLLS})
            set_target_properties(${PKG_TARGET} PROPERTIES SC_SHADER_PACKAGE_DXC_STAGED ON)
        endif()
    endif()

    # Checked by sc_finalize_shader_packages; see the header comment.
    set_property(GLOBAL APPEND PROPERTY SC_SHADER_PACKAGE_TARGETS "${PKG_TARGET}")

    # And the generated dir, so finalize can offer it to the sibling test. Recorded rather than acted on here
    # because a library declares its package before its own test target exists.
    set_property(GLOBAL APPEND PROPERTY SC_SHADER_PACKAGE_GEN_DIRS "${PKG_TARGET}|${PKG_NAME}|${_gen_dir}")
endfunction()

# Fails configure with an actionable message if a package was declared but slib is missing, and hands each
# package's generated header to the sibling <target>-test where there is one.
# Call once, at the bottom of the root CMakeLists.
function(sc_finalize_shader_packages)
    get_property(_pkg_targets GLOBAL PROPERTY SC_SHADER_PACKAGE_TARGETS)
    if(_pkg_targets AND NOT TARGET shaped-shader-library)
        list(REMOVE_DUPLICATES _pkg_targets)
        list(JOIN _pkg_targets ", " _pkg_list)
        message(FATAL_ERROR
            "sc_add_shader_package was called for [${_pkg_list}], but the shaped-shader-library target was "
            "never defined. Check the add_subdirectory order in the root CMakeLists.")
    endif()

    # A package declared by a LIBRARY generates a header that library alone can see, so the generated
    # self_check() -- the parse of the embedded source against the table the generator emitted -- would have
    # nobody to call it.
    # Its sibling test is the one place that both links the library and may link nexus, so it gets the include
    # dir and calls self_check() itself.
    # Here rather than in sc_add_shader_package, because a library declares its package before its test exists.
    #
    # The `-test-web` mirror needs the same dir stated separately rather than inherited.
    # sc_add_nexus_web_runner clones INCLUDE_DIRECTORIES off the base test target at the moment it is called --
    # from inside the library's own CMakeLists, long before this function runs -- so the clone predates the dir
    # and a wasm build fails to find the generated header while every other platform compiles.
    get_property(_gen_dirs GLOBAL PROPERTY SC_SHADER_PACKAGE_GEN_DIRS)
    foreach(_entry IN LISTS _gen_dirs)
        string(REPLACE "|" ";" _parts "${_entry}")
        list(GET _parts 0 _pkg_target)
        list(GET _parts 1 _pkg_name)
        list(GET _parts 2 _pkg_gen_dir)
        foreach(_suffix "-test" "-test-web")
            if(TARGET ${_pkg_target}${_suffix})
                target_include_directories(${_pkg_target}${_suffix} PRIVATE "${_pkg_gen_dir}")
                add_dependencies(${_pkg_target}${_suffix} ${_pkg_target}-${_pkg_name}-shader-package)
            endif()
        endforeach()
    endforeach()
endfunction()
