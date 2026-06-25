# Browser test runners for nexus `*-test` targets (Emscripten only; a no-op elsewhere).
#
# Two-step design:
#   sc_add_nexus_web_runner(<test-target>)  — per library, called from its CMakeLists. Builds
#       `<test-target>-web` as a MODULARIZE'd wasm module (same test sources, exporting the nx_web_* C ABI
#       from web_runner.cc) and registers it so the pages below can load it.
#   sc_finalize_web_runners()               — once, from the top-level CMakeLists after all libs are added.
#       Generates the HTML pages + shared driver into the build root: one per-library page, plus an
#       aggregate `tests-web.html` that loads every library's module and shows them grouped with a grand
#       total. New libraries that call sc_add_nexus_web_runner appear in the aggregate automatically.
#
# The modules are plain `.js` + `.wasm` (no emcc-generated HTML); all UI is our own static driver/template,
# so the per-library and aggregate pages share one renderer. Pages live at the build root and reference
# modules by a path *below* them (libs/...), so a static server rooted there (e.g. emrun) reaches both.

set(SC_NEXUS_WEB_DIR "${CMAKE_CURRENT_LIST_DIR}/../web"
    CACHE INTERNAL "Directory holding the nexus web driver + page template")

# web_runner.cc carries the nx_web_* C ABI. It is compiled straight into every runner (not via libnexus)
# so its EMSCRIPTEN_KEEPALIVE exports survive — as an unreferenced archive member it would be pruned.
set(SC_NEXUS_WEB_RUNNER_SRC "${CMAKE_CURRENT_LIST_DIR}/../src/nexus/web/web_runner.cc"
    CACHE INTERNAL "nexus browser-runner ABI translation unit")

function(sc_add_nexus_web_runner base)
    if(NOT EMSCRIPTEN)
        return()
    endif()

    set(web "${base}-web")
    get_target_property(_srcs ${base} SOURCES)
    get_target_property(_libs ${base} LINK_LIBRARIES)

    add_executable(${web} ${_srcs} "${SC_NEXUS_WEB_RUNNER_SRC}")
    if(_libs)
        target_link_libraries(${web} PRIVATE ${_libs})
    endif()

    # A unique JS factory name per module so several can coexist on the aggregate page.
    string(MAKE_C_IDENTIFIER "nexus_web_${base}" _export)

    # MODULARIZE: emit a factory `<_export>(opts) -> Promise<Module>` instead of a global. INVOKE_RUN=0 so
    # main() never runs (the tests register via static initializers regardless); the page calls the ABI
    # directly. NODERAWFS=0 undoes the node-only filesystem the test presets set globally (breaks in a browser).
    target_link_options(${web} PRIVATE
        "SHELL:-s MODULARIZE=1"
        "SHELL:-s EXPORT_NAME=${_export}"
        "SHELL:-s INVOKE_RUN=0"
        "SHELL:-s EXPORTED_RUNTIME_METHODS=ccall,cwrap"
        "SHELL:-s NODERAWFS=0"
    )

    # Path the generated pages (at the build root) use to load this module's loader script.
    file(RELATIVE_PATH _jsrel "${CMAKE_BINARY_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/${web}.js")

    # Library label shown in the UI: the target without its `-test` suffix (clean-core-test -> clean-core).
    string(REGEX REPLACE "-test$" "" _label "${base}")

    # Register this module for sc_finalize_web_runners. Encoded as label|factory|jsPath (| avoids the
    # ';' CMake-list separator).
    set_property(GLOBAL APPEND PROPERTY SC_WEB_TEST_MODULES "${_label}|${_export}|${_jsrel}")
endfunction()

# Emits a single { } JS object literal for one registered module into ${out_var}.
function(_sc_web_module_literal entry out_var)
    string(REPLACE "|" ";" _parts "${entry}")
    list(GET _parts 0 _label)
    list(GET _parts 1 _factory)
    list(GET _parts 2 _js)
    set(${out_var} "{ label: \"${_label}\", factory: \"${_factory}\", js: \"${_js}\" }" PARENT_SCOPE)
endfunction()

# Writes one page (HTML) for the given title and list of module-literal strings.
function(_sc_write_web_page out_html title)
    set(_literals ${ARGN})
    string(JOIN ",\n    " _joined ${_literals})
    set(NEXUS_WEB_TITLE "${title}")
    set(NEXUS_WEB_MODULES_JS "[\n    ${_joined}\n  ]")
    configure_file("${SC_NEXUS_WEB_DIR}/nexus-web-page.html.in" "${out_html}" @ONLY)
endfunction()

function(sc_finalize_web_runners)
    if(NOT EMSCRIPTEN)
        return()
    endif()

    get_property(_modules GLOBAL PROPERTY SC_WEB_TEST_MODULES)
    if(NOT _modules)
        return()
    endif()

    # Shared renderer next to the pages.
    configure_file("${SC_NEXUS_WEB_DIR}/nexus-web-driver.js" "${CMAKE_BINARY_DIR}/nexus-web-driver.js" COPYONLY)

    # Per-library pages + collect every module literal for the aggregate page.
    set(_all_literals "")
    foreach(_entry IN LISTS _modules)
        _sc_web_module_literal("${_entry}" _lit)
        list(APPEND _all_literals "${_lit}")

        string(REPLACE "|" ";" _parts "${_entry}")
        list(GET _parts 0 _label)
        _sc_write_web_page("${CMAKE_BINARY_DIR}/${_label}-web.html" "${_label}" "${_lit}")
    endforeach()

    # Aggregate page: all libraries at once.
    _sc_write_web_page("${CMAKE_BINARY_DIR}/tests-web.html" "all libraries" ${_all_literals})

    message(STATUS "[nexus] browser test runners: tests-web.html + per-library pages in ${CMAKE_BINARY_DIR}")
endfunction()
