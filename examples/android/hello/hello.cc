// The smallest thing an APK can be: it fills its window with one color, using no GPU.
//
// It proves packaging, deployment and launch on a device, with nothing else that could be wrong.
// `uv run dev.py android run android-hello` installs and starts it.

#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>

namespace
{
constexpr auto tag = "android-hello";

void fill(ANativeWindow* window)
{
    // a format the CPU can write, whatever the window prefers
    ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888);
    auto buffer = ANativeWindow_Buffer{};
    if (ANativeWindow_lock(window, &buffer, nullptr) != 0)
    {
        __android_log_print(ANDROID_LOG_ERROR, tag, "the window could not be locked");
        return;
    }
    auto* const pixels = static_cast<unsigned char*>(buffer.bits);
    for (auto y = 0; y < buffer.height; ++y)
    {
        auto* row = pixels + y * buffer.stride * 4;
        for (auto x = 0; x < buffer.width; ++x)
        {
            row[x * 4 + 0] = 40;
            row[x * 4 + 1] = 120;
            row[x * 4 + 2] = 200;
            row[x * 4 + 3] = 255;
        }
    }
    ANativeWindow_unlockAndPost(window);
    __android_log_print(ANDROID_LOG_INFO, tag, "filled a window of %d x %d", buffer.width, buffer.height);
}
} // namespace

extern "C" __attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity* activity, void*, size_t)
{
    __android_log_print(ANDROID_LOG_INFO, tag, "started");
    activity->callbacks->onNativeWindowCreated = [](ANativeActivity*, ANativeWindow* window) { fill(window); };
    activity->callbacks->onNativeWindowRedrawNeeded = [](ANativeActivity*, ANativeWindow* window) { fill(window); };
}
