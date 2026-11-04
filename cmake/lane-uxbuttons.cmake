# Lane "ux-buttons" (internal plan ux-buttons.md): one token-based button system with in-house icons and one safety model for
# the native app and the web dashboard. Included by CMakeLists.txt (cmake/lane-*.cmake).
# UXB-0 adds the Qt Quick Test runner, UXB-1 the spec generator target and test_uispec, UXB-3a the PlayPlay.Buttons QML
# module (ui-buttons.qrc, unconditionally in PlayPlay) and qmllint_buttons. Nothing is added to PlayPlaySupervisor or
# PlayPlayChannelEngine; tools stay EXCLUDE_FROM_ALL.

# PlayPlay.Buttons (UXB-3a): Play_Play_Native/qml/PlayPlay/Buttons/ compiled in through its own qrc at qrc:/qml/PlayPlay/Buttons/,
# which main.cpp's addImportPath("qrc:/qml") already resolves for `import PlayPlay.Buttons`. Unconditional, because views will
# import it; until one does (UXB-5 / UXB-9 ...) it only adds resources, so the app behaves as before. Its QML imports
# QtQuick.Shapes at run time (Qt Declarative's qmlshapesplugin; scripts/package.ps1 ships QtQuick\Shapes), so nothing links here.
if(TARGET PlayPlay)
    target_sources(PlayPlay PRIVATE ${CMAKE_SOURCE_DIR}/Play_Play_Native/ui-buttons.qrc)
endif()

# gen_uispec (UXB-1): regenerates the button spec's committed outputs in the source tree from assets/ui/buttons.json and
# assets/ui/icons.json (Play_Play_Native/qml/PlayPlay/Buttons/spec.js, web/ui-spec.js, web/icons.svg, web/ui-tokens.css).
# Same as `cmake -P tools/uispec/gen_uispec.cmake`; not part of ALL, and it rewrites only files whose content changed.
add_custom_target(gen_uispec
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_SOURCE_DIR}/tools/uispec/gen_uispec.cmake"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Generating the button spec from assets/ui/buttons.json and assets/ui/icons.json"
    SOURCES "${CMAKE_SOURCE_DIR}/assets/ui/buttons.json" "${CMAKE_SOURCE_DIR}/assets/ui/icons.json"
            "${CMAKE_SOURCE_DIR}/tools/uispec/gen_uispec.cmake"
    VERBATIM)

# PlayPlayButtonGallery (UXB-6): the standalone visual check of the module (tools/buttongallery/main.cpp): every component x
# role x size x state, the icon grid, the shim panel, --screenshot / --zoom / --bench / --check. Built by name only; a
# console executable, so --bench and --check print their numbers. It links the module the way PlayPlay does
# (ui-buttons.qrc) plus gallery.qrc (the page and the three legacy keys it compares); nothing is added to the app targets.
add_executable(PlayPlayButtonGallery EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tools/buttongallery/main.cpp
    ${CMAKE_SOURCE_DIR}/tools/buttongallery/gallery.qrc
    ${CMAKE_SOURCE_DIR}/Play_Play_Native/ui-buttons.qrc)
target_link_libraries(PlayPlayButtonGallery PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
target_compile_definitions(PlayPlayButtonGallery PRIVATE PLAYPLAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

if(PLAYPLAY_BUILD_TESTS)
    # buttongallery_check (UXB-6): every gallery page loads and renders with no QML warning, the icon grid draws every icon
    # of the set by a known name, and no page image is blank. Offscreen with the software renderer, so it is headless and
    # never shows a window. QT_QPA_FONTDIR: the offscreen platform's font database has no fonts of its own, and without
    # Segoe UI the shim panel's widths would be meaningless. The --bench budgets need the GPU and are run by hand
    # (internal plan ux-buttons.md 5.4; `PlayPlayButtonGallery --bench`).
    file(TO_CMAKE_PATH "$ENV{SystemRoot}/Fonts" PP_GALLERY_FONTS)
    add_test(NAME buttongallery_check
        COMMAND PlayPlayButtonGallery --check --screenshot "${CMAKE_BINARY_DIR}/buttongallery_check.png")
    set_tests_properties(buttongallery_check PROPERTIES TIMEOUT 120
        ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1;QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QPA_FONTDIR=${PP_GALLERY_FONTS}")

    # test_uispec (UXB-1): the spec JSON, the icon path syntax, the generated files against a fresh `cmake -P` run (it runs
    # the cmake that configured this tree) and the icon names used in QML / web sources. Built by name only.
    add_executable(test_uispec EXCLUDE_FROM_ALL ${CMAKE_SOURCE_DIR}/tests/test_uispec.cpp)
    target_link_libraries(test_uispec PRIVATE Qt6::Test Qt6::Qml)
    target_compile_definitions(test_uispec PRIVATE PLAYPLAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}" PP_CMAKE_COMMAND="${CMAKE_COMMAND}")
    add_test(NAME test_uispec COMMAND test_uispec)
    set_tests_properties(test_uispec PROPERTIES TIMEOUT 120 ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1")

    find_package(Qt6 QUIET COMPONENTS QuickTest)
    if(TARGET Qt6::QuickTest)
        # test_buttons_qml: Qt Quick Test over every tests/qml/buttons/tst_*.qml, read from the source tree at run time, so a
        # new QML test needs no edit here. The runner defaults to the offscreen platform and the software renderer, so it
        # runs headless and never takes the desktop focus from the live app.
        # It links ui-buttons.qrc, so the tests import PlayPlay.Buttons from qrc:/qml exactly as the app does (rebuild after a
        # module edit).
        add_executable(test_buttons_qml ${CMAKE_SOURCE_DIR}/tests/test_buttons_qml.cpp
            ${CMAKE_SOURCE_DIR}/Play_Play_Native/ui-buttons.qrc)
        target_link_libraries(test_buttons_qml PRIVATE Qt6::QuickTest Qt6::Qml Qt6::Quick Qt6::QuickControls2)
        target_compile_definitions(test_buttons_qml PRIVATE
            QUICK_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/tests/qml/buttons"
            PLAYPLAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
        add_test(NAME test_buttons_qml COMMAND test_buttons_qml)
        set_tests_properties(test_buttons_qml PROPERTIES TIMEOUT 60
            ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1;QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
    endif()

    # qmllint_buttons (UXB-3a): qmllint over every module .qml (globbed, so a new component is linted without an edit here);
    # --max-warnings 0 turns any warning into a failure (info-level notes such as unused imports do not count).
    find_package(Qt6 QUIET COMPONENTS QmlTools)
    file(GLOB PP_BUTTONS_QML CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/Play_Play_Native/qml/PlayPlay/Buttons/*.qml")
    if(TARGET Qt6::qmllint)
        set(PP_QMLLINT "$<TARGET_FILE:Qt6::qmllint>")
    else()
        find_program(PP_QMLLINT NAMES qmllint qmllint6 HINTS "${QT6_INSTALL_PREFIX}/bin" "${QT6_INSTALL_PREFIX}/lib/qt6/bin")
    endif()
    # Off unless PLAYPLAY_QMLLINT_TESTS=ON: on 2026-10-26 this lint ran away to 17 GB and starved the on-air box (see internal plan impl-log).
    option(PLAYPLAY_QMLLINT_TESTS "Register the qmllint test (memory-hungry on the Buttons module)" OFF)
    if(PLAYPLAY_QMLLINT_TESTS AND PP_QMLLINT AND PP_BUTTONS_QML)
        add_test(NAME qmllint_buttons
            COMMAND ${PP_QMLLINT} --max-warnings 0 -I "${CMAKE_SOURCE_DIR}/Play_Play_Native/qml" ${PP_BUTTONS_QML}
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        set_tests_properties(qmllint_buttons PROPERTIES TIMEOUT 120)
    endif()
endif()

if(PLAYPLAY_BUILD_TESTS)
    # test_web_uibuttons (UXB-14): web/ui-buttons-core.js run as an ES module in QJSEngine (no Node): HTML output of the
    # builders, escaping (the a');evil();// case), glyphToIcon, hotkeys, resolvePrefs / validatePrefs, and index.html's
    # load order. Built by name only.
    add_executable(test_web_uibuttons EXCLUDE_FROM_ALL ${CMAKE_SOURCE_DIR}/tests/test_web_uibuttons.cpp)
    target_link_libraries(test_web_uibuttons PRIVATE Qt6::Test Qt6::Qml)
    target_compile_definitions(test_web_uibuttons PRIVATE PLAYPLAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME test_web_uibuttons COMMAND test_web_uibuttons)
    set_tests_properties(test_web_uibuttons PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1")
endif()
