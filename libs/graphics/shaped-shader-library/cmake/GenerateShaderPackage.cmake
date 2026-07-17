# Generates a shader package's C++ from a manifest. Run in script mode by sc_add_shader_package's
# add_custom_command:
#
#   cmake -DMANIFEST=<path> -DOUT_DIR=<dir> -P GenerateShaderPackage.cmake
#
# Only those two scalars come in; everything else is read from the manifest, because a CMake list passed
# through -D would split on its own ';' before the script ever saw it.
#
# Emits, into OUT_DIR:
#   <name>.hh  the typed symbols call sites use, plus package()
#   <name>.cc  the globals, the definition table, and every source file embedded
#   <name>.d   a depfile naming every file read, so editing an .hlsli regenerates
#
# This runs at BUILD time, not configure time. The .cc embeds shader source, so editing a shader must
# regenerate it; doing this at configure time would silently ship a stale copy from an incremental build.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED MANIFEST)
    message(FATAL_ERROR "GenerateShaderPackage: -DMANIFEST=<path> is required")
endif()
if(NOT DEFINED OUT_DIR)
    message(FATAL_ERROR "GenerateShaderPackage: -DOUT_DIR=<dir> is required")
endif()

# ---------------------------------------------------------------------------------------------------
# manifest
# ---------------------------------------------------------------------------------------------------

# The manifest is `key=value` lines; SHADER= repeats, one per declared shader. Written by
# sc_add_shader_package via file(CONFIGURE), so it is copy-if-different and does not churn the build.
file(STRINGS "${MANIFEST}" _manifest_lines)

set(_name "")
set(_namespace "")
set(_source_dir "")
set(_language "")
set(_shaders "")

foreach(_line IN LISTS _manifest_lines)
    if(_line MATCHES "^([A-Z_]+)=(.*)$")
        set(_key "${CMAKE_MATCH_1}")
        set(_value "${CMAKE_MATCH_2}")
        if(_key STREQUAL "NAME")
            set(_name "${_value}")
        elseif(_key STREQUAL "NAMESPACE")
            set(_namespace "${_value}")
        elseif(_key STREQUAL "SOURCE_DIR")
            set(_source_dir "${_value}")
        elseif(_key STREQUAL "LANGUAGE")
            set(_language "${_value}")
        elseif(_key STREQUAL "SHADER")
            list(APPEND _shaders "${_value}")
        endif()
    endif()
endforeach()

# ---------------------------------------------------------------------------------------------------
# validation + parsing
# ---------------------------------------------------------------------------------------------------

# Kept in step with sg::shader_stage (shaped-graphics/compiled_shader.hh). The DSL spells stages exactly
# as the enum does, so the generator emits the enumerator rather than a string for C++ to parse back.
set(_valid_stages
    vertex tessellation_control tessellation_evaluation geometry fragment compute
    raygen closest_hit any_hit miss intersection callable)

set(_valid_languages hlsl)

if(NOT _language IN_LIST _valid_languages)
    message(FATAL_ERROR "shader package '${_name}': unknown LANGUAGE '${_language}'. One of: ${_valid_languages}")
endif()

# Files whose text gets embedded: every declared shader plus, later, its include closure.
set(_files_to_embed "")

# Parsed entries, grouped for emission. Each shader is `path:stage:entry_point`.
set(_stems "")            # unique C++ stem ids, in declaration order
set(_seen_triples "")     # path:stage:entry duplicate guard

foreach(_shader IN LISTS _shaders)
    string(REPLACE ":" ";" _parts "${_shader}")
    list(LENGTH _parts _part_count)
    if(NOT _part_count EQUAL 3)
        message(FATAL_ERROR
            "shader package '${_name}': entry '${_shader}' must be path:stage:entry_point")
    endif()

    list(GET _parts 0 _path)
    list(GET _parts 1 _stage)
    list(GET _parts 2 _entry)

    if(NOT _stage IN_LIST _valid_stages)
        message(FATAL_ERROR
            "shader package '${_name}': entry '${_shader}' has unknown stage '${_stage}'. "
            "Stages are spelled as sg::shader_stage: ${_valid_stages}")
    endif()

    if(NOT EXISTS "${_source_dir}/${_path}")
        message(FATAL_ERROR
            "shader package '${_name}': '${_path}' does not exist under ${_source_dir}")
    endif()

    if("${_shader}" IN_LIST _seen_triples)
        message(FATAL_ERROR "shader package '${_name}': entry '${_shader}' is declared twice")
    endif()
    list(APPEND _seen_triples "${_shader}")

    # A stem is the file name turned into a C++ identifier: post-process/manga_render.hlsl -> manga_render.
    get_filename_component(_stem "${_path}" NAME_WE)
    get_filename_component(_dir "${_path}" DIRECTORY)
    string(REPLACE "-" "_" _stem_id "${_stem}")
    string(REPLACE "-" "_" _dir_id "${_dir}")
    string(REPLACE "/" "_" _dir_id "${_dir_id}")
    if(_dir_id STREQUAL "")
        set(_group "${_stem_id}")
    else()
        set(_group "${_dir_id}_${_stem_id}")
    endif()

    # Two files that collapse to one identifier would emit the same struct twice; say so here rather
    # than in a wall of C++ redefinition errors.
    if("${_group}" IN_LIST _stems)
        if(NOT "${_orig_path_${_group}}" STREQUAL "${_path}")
            message(FATAL_ERROR
                "shader package '${_name}': '${_path}' and '${_orig_path_${_group}}' both map to the C++ "
                "identifier '${_group}'")
        endif()
    else()
        list(APPEND _stems "${_group}")
        set("_orig_path_${_group}" "${_path}")
        set("_stages_${_group}" "")
    endif()

    if(NOT "${_stage}" IN_LIST "_stages_${_group}")
        list(APPEND "_stages_${_group}" "${_stage}")
        set("_entries_${_group}_${_stage}" "")
    endif()
    list(APPEND "_entries_${_group}_${_stage}" "${_entry}")

    if(NOT "${_path}" IN_LIST _files_to_embed)
        list(APPEND _files_to_embed "${_path}")
    endif()
endforeach()

# ---------------------------------------------------------------------------------------------------
# include closure
# ---------------------------------------------------------------------------------------------------

# A shipped binary reads its shaders from the embedded copy, and ssc::dxc has no filesystem fallback for
# #include -- an unresolved one is a hard error. So every file a shader pulls in has to be embedded too,
# not just the entry points. Scanning for `#include "..."` is a deliberate approximation: it does not know
# about #if, so it over-approximates (embeds a file an #ifdef would have skipped). Over-approximating
# costs binary size; under-approximating would break the shipped build, so the bias is the right way up.
set(_scan_queue ${_files_to_embed})
while(_scan_queue)
    list(POP_FRONT _scan_queue _current)
    file(READ "${_source_dir}/${_current}" _text)

    string(REGEX MATCHALL "#[ \t]*include[ \t]*\"[^\"]+\"" _includes "${_text}")
    foreach(_include IN LISTS _includes)
        string(REGEX REPLACE "^#[ \t]*include[ \t]*\"([^\"]+)\"$" "\\1" _include_path "${_include}")

        # Mirrors slib::shader_library's resolution: next to the including file, then the package root.
        # A path that resolves to neither lives in another mount (a shared library) and is not ours to embed.
        get_filename_component(_current_dir "${_current}" DIRECTORY)
        if(_current_dir STREQUAL "")
            set(_sibling "${_include_path}")
        else()
            set(_sibling "${_current_dir}/${_include_path}")
        endif()

        set(_resolved "")
        if(EXISTS "${_source_dir}/${_sibling}")
            set(_resolved "${_sibling}")
        elseif(EXISTS "${_source_dir}/${_include_path}")
            set(_resolved "${_include_path}")
        endif()

        if(_resolved AND NOT "${_resolved}" IN_LIST _files_to_embed)
            list(APPEND _files_to_embed "${_resolved}")
            list(APPEND _scan_queue "${_resolved}")
        endif()
    endforeach()
endwhile()

# ---------------------------------------------------------------------------------------------------
# emit the header
# ---------------------------------------------------------------------------------------------------

string(REPLACE "::" ";" _namespace_parts "${_namespace}")

set(_hh "// This file is auto-generated by sc_add_shader_package. Do not edit.\n")
string(APPEND _hh "#pragma once\n\n")
string(APPEND _hh "#include <shaped-shader-library/fwd.hh>\n")
string(APPEND _hh "#include <shaped-shader-library/shader_asset.hh>\n")
string(APPEND _hh "#include <shaped-shader-library/shader_package.hh>\n\n")
string(APPEND _hh "namespace ${_namespace}\n{\n")

foreach(_group IN LISTS _stems)
    string(APPEND _hh "/// ${_orig_path_${_group}}\n")
    string(APPEND _hh "struct ${_group}_t\n{\n")
    foreach(_stage IN LISTS "_stages_${_group}")
        string(APPEND _hh "    struct\n    {\n")
        foreach(_entry IN LISTS "_entries_${_group}_${_stage}")
            string(APPEND _hh "        slib::shader_asset_handle ${_entry};\n")
        endforeach()
        string(APPEND _hh "    } ${_stage};\n")
    endforeach()
    string(APPEND _hh "};\n")
    string(APPEND _hh "extern ${_group}_t ${_group};\n\n")
endforeach()

string(APPEND _hh "/// Pass to slib::shader_library::add_package. The handles above are null until you do.\n")
string(APPEND _hh "slib::shader_package const& package();\n")
string(APPEND _hh "} // namespace ${_namespace}\n")

# ---------------------------------------------------------------------------------------------------
# emit the source
# ---------------------------------------------------------------------------------------------------

set(_cc "// This file is auto-generated by sc_add_shader_package. Do not edit.\n\n")
string(APPEND _cc "#include \"${_name}.hh\"\n\n")
string(APPEND _cc "#include <shaped-graphics/compiled_shader.hh>\n\n")

foreach(_group IN LISTS _stems)
    string(APPEND _cc "${_namespace}::${_group}_t ${_namespace}::${_group};\n")
endforeach()
string(APPEND _cc "\nnamespace\n{\n")

# Sources are embedded as raw string literals so a shipped binary carries its own shaders and needs no
# source tree. A delimiter keeps a shader containing )" from ending the literal early.
set(_embed_index 0)
set(_embedded_entries "")
foreach(_file IN LISTS _files_to_embed)
    file(READ "${_source_dir}/${_file}" _text)
    string(APPEND _cc "constexpr char const* k_source_${_embed_index} = R\"slibsrc(${_text})slibsrc\";\n")
    list(APPEND _embedded_entries "{.path = \"${_file}\", .text = k_source_${_embed_index}}")
    math(EXPR _embed_index "${_embed_index} + 1")
endforeach()

string(APPEND _cc "\nconstexpr slib::embedded_file k_embedded_files[] = {\n")
foreach(_entry IN LISTS _embedded_entries)
    string(APPEND _cc "    ${_entry},\n")
endforeach()
string(APPEND _cc "};\n")

# The definition table carries the declared path through verbatim rather than rebuilding it from the
# folder and stem, and emits the sg::shader_stage enumerator rather than a string to parse back.
string(APPEND _cc "\nslib::shader_definition const k_definitions[] = {\n")
foreach(_group IN LISTS _stems)
    foreach(_stage IN LISTS "_stages_${_group}")
        foreach(_entry IN LISTS "_entries_${_group}_${_stage}")
            string(APPEND _cc "    {.path = \"${_orig_path_${_group}}\",\n")
            string(APPEND _cc "     .stage = sg::shader_stage::${_stage},\n")
            string(APPEND _cc "     .entry_point = \"${_entry}\",\n")
            string(APPEND _cc "     .asset = &${_namespace}::${_group}.${_stage}.${_entry}},\n")
        endforeach()
    endforeach()
endforeach()
string(APPEND _cc "};\n")
string(APPEND _cc "} // namespace\n\n")

# The absolute source dir is baked in. A dev build finds it and hot-reloads; a shipped build does not and
# falls back to the embedded copy above. No mode flag, no probing for "am I installed".
string(APPEND _cc "slib::shader_package const& ${_namespace}::package()\n{\n")
string(APPEND _cc "    static slib::shader_package const pkg = {\n")
string(APPEND _cc "        .name = \"${_name}\",\n")
string(APPEND _cc "        .language = slib::shader_language::${_language},\n")
string(APPEND _cc "        .source_dir = \"${_source_dir}\",\n")
string(APPEND _cc "        .embedded_files = k_embedded_files,\n")
string(APPEND _cc "        .definitions = k_definitions,\n")
string(APPEND _cc "    };\n")
string(APPEND _cc "    return pkg;\n")
string(APPEND _cc "}\n")

# ---------------------------------------------------------------------------------------------------
# write
# ---------------------------------------------------------------------------------------------------

# file(CONFIGURE) is copy-if-different: an unchanged package must not retrigger the compile that
# consumes these. @ONLY, and the content is passed via a variable, so nothing in a shader is expanded.
set(_hh_content "${_hh}")
set(_cc_content "${_cc}")
file(CONFIGURE OUTPUT "${OUT_DIR}/${_name}.hh" CONTENT "@_hh_content@" @ONLY)
file(CONFIGURE OUTPUT "${OUT_DIR}/${_name}.cc" CONTENT "@_cc_content@" @ONLY)

# Depfile: every file we read. This is what makes editing an .hlsli regenerate the package -- DEPENDS
# alone only covers the entry points named in the manifest, and the include closure is discovered here.
set(_depfile "${OUT_DIR}/${_name}.hh:")
foreach(_file IN LISTS _files_to_embed)
    string(APPEND _depfile " \\\n  ${_source_dir}/${_file}")
endforeach()
string(APPEND _depfile "\n")
file(WRITE "${OUT_DIR}/${_name}.d" "${_depfile}")
