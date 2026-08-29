#include <nexus/run.hh>

// The Vulkan loader and the ICD it dlopens allocate per-process state on their own threads and never free it.
// That is a leak LeakSanitizer can see and we cannot annotate: it happens on a driver-spawned thread, so cc::leak_scope
// (which disables reporting for the calling thread only) does not reach it, and there is no pointer of ours to name.
// A suppression is what is left.
// It is matched loosely because the frames land in a dlopen'd module with no symbols, leaving __pthread_once — the one-time init the driver runs — as the only stable frame to key on.
//
// Deliberately scoped to this binary rather than the whole build, so a leak on a driver thread stays reportable
// everywhere else, and remove it the moment the driver stops doing this.
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SG_VULKAN_TEST_SUPPRESS_DRIVER_LEAKS 1
#endif
#elif defined(__SANITIZE_ADDRESS__)
#define SG_VULKAN_TEST_SUPPRESS_DRIVER_LEAKS 1
#endif
#ifndef SG_VULKAN_TEST_SUPPRESS_DRIVER_LEAKS
#define SG_VULKAN_TEST_SUPPRESS_DRIVER_LEAKS 0
#endif

#if SG_VULKAN_TEST_SUPPRESS_DRIVER_LEAKS
extern "C" char const* __lsan_default_suppressions()
{
    return "leak:__pthread_once\n";
}
#endif

int main(int argc, char** argv)
{
    return nx::run(argc, argv);
}
