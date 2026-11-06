# Packages one native library into an APK: run by sc_add_android_apk through `cmake -P`, as a build step.
# Takes TOOLS and PLATFORM (SDK folders), ABI, LIBRARY, STRIP, WORK (holding AndroidManifest.xml) and OUTPUT.

cmake_minimum_required(VERSION 3.28)

get_filename_component(library_name "${LIBRARY}" NAME)
set(staged "${WORK}/staged.apk")
set(aligned "${WORK}/aligned.apk")
file(REMOVE "${staged}" "${aligned}" "${OUTPUT}")

execute_process(
    COMMAND "${TOOLS}/aapt2" link -o "${staged}" -I "${PLATFORM}/android.jar" --manifest "${WORK}/AndroidManifest.xml"
        --version-code 1 --version-name 1.0
    COMMAND_ERROR_IS_FATAL ANY)

# aapt stores a path as it is given, so it runs from the folder the APK's own layout starts at
file(MAKE_DIRECTORY "${WORK}/root/lib/${ABI}")
file(COPY_FILE "${LIBRARY}" "${WORK}/root/lib/${ABI}/${library_name}")
if(STRIP)
    execute_process(COMMAND "${STRIP}" --strip-unneeded "${WORK}/root/lib/${ABI}/${library_name}" COMMAND_ERROR_IS_FATAL ANY)
endif()
execute_process(
    COMMAND "${TOOLS}/aapt" add "${staged}" "lib/${ABI}/${library_name}"
    WORKING_DIRECTORY "${WORK}/root"
    OUTPUT_QUIET
    COMMAND_ERROR_IS_FATAL ANY)

# 16 KiB pages, which a current device may use
execute_process(COMMAND "${TOOLS}/zipalign" -f -P 16 4 "${staged}" "${aligned}" COMMAND_ERROR_IS_FATAL ANY)

find_program(keytool keytool)
find_program(java java)
if(NOT keytool OR NOT java)
    message(WARNING "no Java runtime, so ${OUTPUT} is unsigned, and a device refuses to install it")
    file(COPY_FILE "${aligned}" "${OUTPUT}")
    return()
endif()

# The conventional debug key, shared with every other Android build tool on this machine.
# Kept out of the build folder, because a device refuses an update signed by a different key.
if(CMAKE_HOST_WIN32)
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}" home)
else()
    set(home "$ENV{HOME}")
endif()
set(keystore "${home}/.android/debug.keystore")
if(NOT EXISTS "${keystore}")
    file(MAKE_DIRECTORY "${home}/.android")
    execute_process(
        COMMAND "${keytool}" -genkeypair -keystore "${keystore}" -storepass android -keypass android
            -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US"
        OUTPUT_QUIET ERROR_QUIET
        COMMAND_ERROR_IS_FATAL ANY)
endif()

if(CMAKE_HOST_WIN32)
    set(apksigner "${TOOLS}/apksigner.bat")
else()
    set(apksigner "${TOOLS}/apksigner")
endif()
execute_process(
    COMMAND "${apksigner}" sign --ks "${keystore}" --ks-pass pass:android --key-pass pass:android
        --out "${OUTPUT}" "${aligned}"
    COMMAND_ERROR_IS_FATAL ANY)
