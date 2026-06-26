# Vendored third-party dependencies. Each lives in-tree under extern/<lib>/ at a
# pinned commit (see the per-lib vendor-*.py). Each SC_USE_VENDORED_<LIB> defaults
# ON and builds the in-tree copy; turning one OFF means the embedding project must
# already provide a target of that name.
#
# Add new vendored deps here so the root CMakeLists.txt only does add_subdirectory(extern).

option(SC_USE_VENDORED_MIMALLOC "Build the in-tree vendored mimalloc (OFF expects an external `mimalloc` target)" ON)
option(SC_USE_VENDORED_XXHASH "Build the in-tree vendored xxHash (OFF expects an external `xxhash` target)" ON)

if(SC_USE_VENDORED_MIMALLOC)
    add_subdirectory(mimalloc)
endif()

if(SC_USE_VENDORED_XXHASH)
    add_subdirectory(xxhash)
endif()
