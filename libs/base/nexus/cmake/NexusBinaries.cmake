# Which executables are nexus binaries, and what each one carries: tests, examples, or a tool's apps and commands.
#
# A binary's name used to say this — `*-test`, `*-example` — and dev.py found test binaries by the suffix.
# A tool that carries its own tests is named for the tool, so the kinds are recorded instead, and dev.py reads them
# from the manifest this writes at the end of configure.
# The CMake File API reports no custom target property, which is why the manifest exists at all.
#
#   sc_nexus_binary(<target> KINDS <tests|examples|tool|stub>... [TIMEOUT <seconds>])
#       Records `target` as a nexus binary holding those kinds. Call it once per target, after add_executable.
#       `stub` marks a stand-in that only says why its example is not built here: no nexus runner, so no test run starts it.
#       TIMEOUT is how long a test run of it may take, when that is more than dev.py's default.
#       It is for a binary whose cost the machine sets rather than the code, such as one that runs WARP on a GPU-less host.
#       On Windows the binary's manifest makes UTF-8 its code page, so argv arrives as UTF-8 and a test name outside ASCII matches.
#
#   sc_write_nexus_binary_manifest()
#       Writes <build>/nexus-binaries.json at generate time, the end of configure.
#       Called once, at the end of the top-level CMakeLists.
#       `binaries` holds each binary's kinds and `timeouts` the declared TIMEOUTs.
#       `paths` maps every nexus binary to its absolute output file, which is how a tool outside dev.py finds one — the SGL editor extension finds `sgl` this way.

function(sc_nexus_binary target)
    cmake_parse_arguments(PARSE_ARGV 1 NB "" "TIMEOUT" "KINDS")
    if(NOT NB_KINDS)
        message(FATAL_ERROR "sc_nexus_binary(${target}): KINDS is required")
    endif()
    if(NB_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "sc_nexus_binary(${target}): unexpected arguments: ${NB_UNPARSED_ARGUMENTS}")
    endif()
    foreach(_kind IN LISTS NB_KINDS)
        if(NOT _kind MATCHES "^(tests|examples|tool|stub)$")
            message(FATAL_ERROR "sc_nexus_binary(${target}): unknown kind '${_kind}' — one of tests, examples, tool, stub")
        endif()
    endforeach()

    list(JOIN NB_KINDS "," _joined)
    set_property(GLOBAL APPEND PROPERTY SC_NEXUS_BINARIES "${target}=${_joined}")

    if(WIN32)
        target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/utf8-code-page.manifest")
    endif()

    if(DEFINED NB_TIMEOUT)
        if(NOT NB_TIMEOUT MATCHES "^[0-9]+$" OR NB_TIMEOUT EQUAL 0)
            message(FATAL_ERROR "sc_nexus_binary(${target}): TIMEOUT must be a positive number of seconds, not '${NB_TIMEOUT}'")
        endif()
        set_property(GLOBAL APPEND PROPERTY SC_NEXUS_BINARY_TIMEOUTS "${target}=${NB_TIMEOUT}")
    endif()
endfunction()

# Whether `all` leaves `target` out: its own property, or that of any directory from its own up to the top.
# The directory property is set only where add_subdirectory(... EXCLUDE_FROM_ALL) was called, not on the directories below it.
function(_sc_nexus_excluded_from_all target out)
    get_target_property(_excluded ${target} EXCLUDE_FROM_ALL)
    get_target_property(_dir ${target} SOURCE_DIR)
    while(NOT _excluded AND _dir)
        get_property(_excluded DIRECTORY "${_dir}" PROPERTY EXCLUDE_FROM_ALL)
        get_property(_dir DIRECTORY "${_dir}" PROPERTY PARENT_DIRECTORY)
    endwhile()
    set(${out} ${_excluded} PARENT_SCOPE)
endfunction()

function(sc_write_nexus_binary_manifest)
    get_property(_binaries GLOBAL PROPERTY SC_NEXUS_BINARIES)

    set(_entries "")
    foreach(_binary IN LISTS _binaries)
        string(REPLACE "=" ";" _parts "${_binary}")
        list(GET _parts 0 _name)
        list(GET _parts 1 _kinds)
        # A binary `all` does not build carries no tests a test run could start: a tool of shaped-core's, consumed EXCLUDE_FROM_ALL.
        # Its other kinds and its path stay, since a tool built on demand (`sgl`) is still found through the manifest.
        _sc_nexus_excluded_from_all(${_name} _excluded)
        if(_excluded)
            string(REPLACE "," ";" _kind_list "${_kinds}")
            list(REMOVE_ITEM _kind_list tests)
            if(NOT _kind_list)
                continue()
            endif()
            list(JOIN _kind_list "," _kinds)
        endif()
        string(REPLACE "," "\", \"" _kinds_json "${_kinds}")
        list(APPEND _entries "    \"${_name}\": [\"${_kinds_json}\"]")
    endforeach()
    list(JOIN _entries ",\n" _body)

    get_property(_timeouts GLOBAL PROPERTY SC_NEXUS_BINARY_TIMEOUTS)
    set(_timeout_entries "")
    foreach(_timeout IN LISTS _timeouts)
        string(REPLACE "=" ";" _parts "${_timeout}")
        list(GET _parts 0 _name)
        list(GET _parts 1 _secs)
        list(APPEND _timeout_entries "    \"${_name}\": ${_secs}")
    endforeach()
    list(JOIN _timeout_entries ",\n" _timeout_body)

    set(_path_entries "")
    foreach(_binary IN LISTS _binaries)
        string(REPLACE "=" ";" _parts "${_binary}")
        list(GET _parts 0 _name)
        list(APPEND _path_entries "    \"${_name}\": \"$<TARGET_FILE:${_name}>\"")
    endforeach()
    list(JOIN _path_entries ",\n" _path_body)

    # file(GENERATE) because the paths are generator expressions; it rewrites the file only when its content changed.
    # Every preset uses the single-config Ninja generator, so one OUTPUT without $<CONFIG> in its name is fine.
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/nexus-binaries.json"
        CONTENT "{\n  \"binaries\": {\n${_body}\n  },\n  \"timeouts\": {\n${_timeout_body}\n  },\n  \"paths\": {\n${_path_body}\n  }\n}\n")
endfunction()
