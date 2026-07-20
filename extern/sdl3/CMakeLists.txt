cmake_minimum_required(VERSION 3.28)

# SDL3: the OS window and event backend behind shaped-rendering's sr::window.
# Not committed — its source tree is ~50 MB across several thousand files.
# fetch-sdl3.py downloads the pinned release into .install/ on demand.
# dev.py runs it per configure; see tools/dev/lib/pipeline/prereqs.py.
# Here we build that tree, the same shape as the fetched Zydis.
set(SDL3_INSTALL ${CMAKE_CURRENT_SOURCE_DIR}/.install)

if(NOT EXISTS "${SDL3_INSTALL}/pin.txt")
    message(FATAL_ERROR
        "SDL3 is not installed (${SDL3_INSTALL} is missing). Fetch it once with:\n"
        "    uv run extern/sdl3/fetch-sdl3.py\n"
        "dev.py does this automatically; see tools/dev/lib/pipeline/prereqs.py.")
endif()

# SDL's build is driven by ~100 options, most defaulting to "on when SDL is the top-level project".
# It is not, here, so most of these already hold.
# We set them anyway, FORCEd, so the build we get never depends on SDL's notion of who is top-level, and so a
# stale cache entry cannot flip one back on.
# Grouped by what the answer buys us.

# Linkage: shared.
# Dynamic is what upstream recommends, tests and supports.
# It also keeps SDL's several hundred TUs out of every static consumer's archive.
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)

# None of SDL's own build furniture.
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE) # the SDL3_test helper library
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE) # we never install SDL; the runtime is staged next to consumers
set(SDL_INSTALL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_UNINSTALL OFF CACHE BOOL "" FORCE) # keeps an `uninstall` target out of our build graph
set(SDL_CCACHE OFF CACHE BOOL "" FORCE) # our toolchain decides that, not SDL's

# Subsystems.
# We want a window and its events; the keyboard, mouse and clipboard ride along with video.
# Everything else is dead weight, and each one off is fewer system libraries to find on Linux.
# Haptic must stay off while joystick is — SDL errors out on that combination.
set(SDL_VIDEO ON CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_POWER OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
set(SDL_TRAY OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE) # SDL's own 2D renderer — we have shaped-graphics
set(SDL_GPU OFF CACHE BOOL "" FORCE) # SDL's own GPU abstraction — likewise

# The dummy video driver is what lets a window test run unattended on a machine with no display.
# On deliberately, not by default.
set(SDL_DUMMYVIDEO ON CACHE BOOL "" FORCE)
set(SDL_OFFSCREEN ON CACHE BOOL "" FORCE)

# Graphics-API loaders.
# SDL needs these only to hand out a GL/Vulkan/Metal-capable window, which we never ask for.
# Our swapchain comes from shaped-graphics and wants nothing but the native handle.
set(SDL_OPENGL OFF CACHE BOOL "" FORCE)
set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
set(SDL_METAL OFF CACHE BOOL "" FORCE)

# Both Linux window systems stay in — which one a machine runs is a runtime fact.
# SDL loads them dynamically, so a machine missing one is fine at build time and at run time.
set(SDL_X11 ON CACHE BOOL "" FORCE)
set(SDL_WAYLAND ON CACHE BOOL "" FORCE)

# SYSTEM marks every target SDL defines as a system include, so <SDL3/SDL.h> cannot trip the consuming TU's
# warnings — the same intent as the SYSTEM include directories on zydis and dxc.
# EXCLUDE_FROM_ALL keeps SDL out of a plain `cmake --build .`.
# It is still built for whoever links it.
add_subdirectory("${SDL3_INSTALL}" "${CMAKE_CURRENT_BINARY_DIR}/sdl3-build" SYSTEM EXCLUDE_FROM_ALL)
