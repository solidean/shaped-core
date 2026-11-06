# An APK with no GPU code, the first thing to run on a device: dev.py android run android-hello
include(AndroidApk)

sc_add_android_apk(android-hello
    PACKAGE dev.shapedcode.hello
    LABEL   "shaped-core hello"
    SOURCES hello.cc
)
