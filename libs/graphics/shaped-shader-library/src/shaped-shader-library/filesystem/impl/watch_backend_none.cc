#include <shaped-shader-library/filesystem/impl/watch_backend.hh>

// No OS file-watch backend on this platform yet — Linux's inotify and macOS's FSEvents are the next two.
// real_filesystem then reports that it cannot notify, and the reload watcher polls exactly as it always
// has. That is why this is a two-line file rather than a hole in the feature.

std::unique_ptr<slib::impl::watch_backend> slib::impl::create_watch_backend()
{
    return nullptr;
}
