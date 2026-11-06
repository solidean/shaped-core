# sc_add_android_apk: a native library packaged as an installable APK, with no gradle and no Java code.
#
#     sc_add_android_apk(android-hello PACKAGE dev.shapedcode.hello LABEL "shaped-core hello" SOURCES hello.cc)
#
# The library exports ANativeActivity_onCreate, and Android's own NativeActivity loads it.
# `<target>-apk` writes build/<preset>/apk/<target>.apk, and <target>.package beside it, which `dev.py android run` reads.
# Packaging needs ANDROID_HOME, which `dev.py install android-sdk` provides; without it only the library is built.
# Signing also needs a Java runtime, for keytool and apksigner, and without one the APK stays unsigned and cannot install.

get_filename_component(_sc_android_apk_dir "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)

function(sc_add_android_apk target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "PACKAGE;LABEL" "SOURCES;LINK")

    add_library(${target} SHARED ${arg_SOURCES})
    target_link_libraries(${target} PRIVATE android log ${arg_LINK})

    if(NOT DEFINED ENV{ANDROID_HOME})
        message(STATUS "android: no ANDROID_HOME (uv run dev.py install android-sdk), so ${target} gets no APK")
        return()
    endif()
    file(TO_CMAKE_PATH "$ENV{ANDROID_HOME}" sdk)

    # the newest of each, since the install pins exactly one
    file(GLOB build_tools LIST_DIRECTORIES true "${sdk}/build-tools/*")
    file(GLOB platforms LIST_DIRECTORIES true "${sdk}/platforms/*")
    list(SORT build_tools COMPARE NATURAL ORDER DESCENDING)
    list(SORT platforms COMPARE NATURAL ORDER DESCENDING)
    if(NOT build_tools OR NOT platforms)
        message(STATUS "android: ${sdk} holds no build-tools or platform, so ${target} gets no APK")
        return()
    endif()
    list(GET build_tools 0 tools)
    list(GET platforms 0 platform)

    set(work "${CMAKE_BINARY_DIR}/apk/${target}.work")
    set(apk "${CMAKE_BINARY_DIR}/apk/${target}.apk")
    set(PACKAGE "${arg_PACKAGE}")
    set(LABEL "${arg_LABEL}")
    set(LIBRARY_NAME "${target}")
    configure_file("${_sc_android_apk_dir}/AndroidManifest.xml.in" "${work}/AndroidManifest.xml" @ONLY)
    file(WRITE "${CMAKE_BINARY_DIR}/apk/${target}.package" "${arg_PACKAGE}")

    add_custom_command(
        OUTPUT "${apk}"
        COMMAND "${CMAKE_COMMAND}"
            "-DTOOLS=${tools}"
            "-DPLATFORM=${platform}"
            "-DABI=${ANDROID_ABI}"
            "-DLIBRARY=$<TARGET_FILE:${target}>"
            "-DSTRIP=${CMAKE_STRIP}"
            "-DWORK=${work}"
            "-DOUTPUT=${apk}"
            -P "${_sc_android_apk_dir}/AndroidApkPackage.cmake"
        DEPENDS ${target} "${work}/AndroidManifest.xml" "${_sc_android_apk_dir}/AndroidApkPackage.cmake"
        COMMENT "Packaging ${target}.apk"
        VERBATIM
    )
    add_custom_target(${target}-apk ALL DEPENDS "${apk}")
endfunction()
