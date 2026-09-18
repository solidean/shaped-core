# Staging the DLLs an executable loads into the directory it runs from — Windows only, since elsewhere the loader
# finds shared libraries through the rpath and nothing needs copying.
#
# **Several executables share one output directory**, and each stages its DLLs after it links.
# The shaped-viewer examples are seven of them, linking in parallel into one directory, each copying the same SDL3.dll
# and dxcompiler.dll there.
# `cmake -E copy_if_different` writes the destination in place, and on Windows two processes writing one file collide:
# CI failed an example's build on exactly that, with a source DLL that had been built hundreds of steps earlier.
#
# So nothing here writes a shared file.
# Each DLL is copied to a temp file named for the executable staging it, then renamed into place.
# A rename replaces the destination whole rather than writing into it, so two stagers can only race to install the same
# bytes — and when a rename loses to one that already put identical bytes there, that is success.
#
# One file, two roles: included, it defines sc_stage_runtime_dlls; run with -P, it is the staging step itself.

if(CMAKE_SCRIPT_MODE_FILE)
    # ---- the staging step, run as a POST_BUILD command ----
    file(READ "${SC_STAGE_LIST}" _dlls)
    string(STRIP "${_dlls}" _dlls)

    foreach(_src IN LISTS _dlls)
        if(_src STREQUAL "")
            continue()
        endif()

        get_filename_component(_name "${_src}" NAME)
        set(_dst "${SC_STAGE_DEST}/${_name}")

        if(EXISTS "${_dst}")
            execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${_src}" "${_dst}" RESULT_VARIABLE _differs)
            if(_differs EQUAL 0)
                continue()
            endif()
        endif()

        set(_tmp "${_dst}.${SC_STAGE_TAG}.staging")
        file(COPY_FILE "${_src}" "${_tmp}")

        # A rename can still fail while something else holds the destination — another stager's rename, or a scanner
        # opening a freshly written DLL — so it is retried briefly rather than failing the build on the first refusal.
        set(_installed FALSE)
        foreach(_attempt RANGE 1 50)
            file(RENAME "${_tmp}" "${_dst}" RESULT _rename)
            if(_rename EQUAL 0)
                set(_installed TRUE)
                break()
            endif()

            # Another stager may have installed these very bytes meanwhile, which is all this step wanted.
            execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${_src}" "${_dst}" RESULT_VARIABLE _differs)
            if(_differs EQUAL 0)
                file(REMOVE "${_tmp}")
                set(_installed TRUE)
                break()
            endif()

            execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.1)
        endforeach()

        if(NOT _installed)
            file(REMOVE "${_tmp}")
            message(FATAL_ERROR "could not stage ${_name} into ${SC_STAGE_DEST}: ${_rename}")
        endif()
    endforeach()
    return()
endif()

# ---- the helper, when included ----

# Stages `DLLS` beside `target` after it links; a no-op off Windows.
# `DLLS` may be a list or a generator expression — $<TARGET_RUNTIME_DLLS:...> is the usual one — and may be empty.
# `TAG` names this staging among others on the same target, since each gets its own temp files.
function(sc_stage_runtime_dlls target)
    if(NOT WIN32)
        return()
    endif()

    cmake_parse_arguments(PARSE_ARGV 1 STAGE "" "TAG" "DLLS")
    if(NOT STAGE_TAG)
        set(STAGE_TAG "${target}")
    endif()

    # **The list travels through a file, never the command line.**
    # Ninja runs a POST_BUILD command inside a second `cmd /C "..."`, whose nested quotes flip the outer shell's quoting —
    # so whatever joins the list reaches cmd bare, and '|' there is a pipe: CI ran the second DLL's path as a command.
    # No separator survives a line cmd re-parses, so the list is written at generate time and read back by the step.
    set(_list "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${STAGE_TAG}.$<CONFIG>.runtime-dlls")
    file(GENERATE OUTPUT "${_list}" CONTENT "${STAGE_DLLS}")

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DSC_STAGE_LIST=${_list}"
            "-DSC_STAGE_DEST=$<TARGET_FILE_DIR:${target}>"
            "-DSC_STAGE_TAG=${STAGE_TAG}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
        VERBATIM
    )
endfunction()
