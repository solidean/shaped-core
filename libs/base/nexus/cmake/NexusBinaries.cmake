# Which executables are nexus binaries, and what each one carries: tests, examples, or a tool's apps and commands.
#
# A binary's name used to say this — `*-test`, `*-example` — and dev.py found test binaries by the suffix.
# A tool that carries its own tests is named for the tool, so the kinds are recorded instead, and dev.py reads them
# from the manifest this writes at the end of configure.
# The CMake File API reports no custom target property, which is why the manifest exists at all.
#
#   sc_nexus_binary(<target> KINDS <tests|examples|tool>...)
#       Records `target` as a nexus binary holding those kinds. Call it once per target, after add_executable.
#
#   sc_write_nexus_binary_manifest()
#       Writes <build>/nexus-binaries.json. Called once, at the end of the top-level CMakeLists.

function(sc_nexus_binary target)
    cmake_parse_arguments(PARSE_ARGV 1 NB "" "" "KINDS")
    if(NOT NB_KINDS)
        message(FATAL_ERROR "sc_nexus_binary(${target}): KINDS is required")
    endif()
    if(NB_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "sc_nexus_binary(${target}): unexpected arguments: ${NB_UNPARSED_ARGUMENTS}")
    endif()
    foreach(_kind IN LISTS NB_KINDS)
        if(NOT _kind MATCHES "^(tests|examples|tool)$")
            message(FATAL_ERROR "sc_nexus_binary(${target}): unknown kind '${_kind}' — one of tests, examples, tool")
        endif()
    endforeach()

    list(JOIN NB_KINDS "," _joined)
    set_property(GLOBAL APPEND PROPERTY SC_NEXUS_BINARIES "${target}=${_joined}")
endfunction()

function(sc_write_nexus_binary_manifest)
    get_property(_binaries GLOBAL PROPERTY SC_NEXUS_BINARIES)

    set(_entries "")
    foreach(_binary IN LISTS _binaries)
        string(REPLACE "=" ";" _parts "${_binary}")
        list(GET _parts 0 _name)
        list(GET _parts 1 _kinds)
        string(REPLACE "," "\", \"" _kinds_json "${_kinds}")
        list(APPEND _entries "    \"${_name}\": [\"${_kinds_json}\"]")
    endforeach()
    list(JOIN _entries ",\n" _body)

    # file(CONFIGURE) is copy-if-different, so a configure that changes nothing leaves the manifest's mtime alone.
    file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/nexus-binaries.json"
        CONTENT "{\n  \"binaries\": {\n${_body}\n  }\n}\n" @ONLY)
endfunction()
