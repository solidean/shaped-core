cmake_minimum_required(VERSION 3.28)
project(sqlite LANGUAGES C)

# SQLite: the embedded SQL engine behind babel-serializer's babel::sqlite format. Not committed —
# its amalgamated sqlite3.c is ~9.5 MB of generated C — so fetch-sqlite.py downloads it into
# .install/ on demand (dev.py runs it per configure; see tools/dev/lib/pipeline/prereqs.py).
# Here we build that single TU into a static lib, the same shape as the vendored xxHash.
set(SQLITE_INSTALL ${CMAKE_CURRENT_SOURCE_DIR}/.install)

if(NOT EXISTS "${SQLITE_INSTALL}/pin.txt")
    message(FATAL_ERROR
        "SQLite is not installed (${SQLITE_INSTALL} is missing). Fetch it once with:\n"
        "    uv run extern/sqlite/fetch-sqlite.py\n"
        "dev.py does this automatically; see tools/dev/lib/pipeline/prereqs.py.")
endif()

add_library(sqlite STATIC ${SQLITE_INSTALL}/src/sqlite3.c)

# SYSTEM so sqlite3.h does not trip the consuming TU's warnings.
target_include_directories(sqlite SYSTEM PUBLIC ${SQLITE_INSTALL}/include)

# Compile-time configuration of the engine. All PRIVATE — internal to this TU, nothing leaks
# through sqlite3.h to consumers.
target_compile_definitions(sqlite PRIVATE
    # Honor the repo-wide threading switch: the serialized threadsafe build when threads are on,
    # the fully single-threaded build (no mutexes) when SC_THREADS is off (the singlethreaded-* presets).
    SQLITE_THREADSAFE=$<IF:$<BOOL:${SC_THREADS}>,1,0>
    SQLITE_ENABLE_FTS5    # full-text search
    SQLITE_ENABLE_RTREE   # R*Tree spatial index
    SQLITE_DQS=0          # reject double-quoted string literals — a common footgun, off in modern setups
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_OMIT_DEPRECATED)

# Third-party code: silence its warnings, hide its symbols (statically absorbed).
if (MSVC)
    target_compile_options(sqlite PRIVATE /W0)
else()
    target_compile_options(sqlite PRIVATE -w -fvisibility=hidden)
endif()

set_property(TARGET sqlite PROPERTY POSITION_INDEPENDENT_CODE ON)
