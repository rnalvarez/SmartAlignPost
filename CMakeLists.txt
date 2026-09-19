cmake_minimum_required(VERSION 3.25)
project(SmartAlignPost VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(SAP_BUILD_VST3 "Build the VST3 plug-in" ON)
option(SAP_BUILD_TESTS "Build the DSP unit tests" ON)
option(SAP_BUILD_PROTOTYPE "Build the REAPER prototype CLI" ON)

# The VST3 SDK is intentionally kept as a Git submodule rather than copied into
# this repository. This keeps the public repository small and preserves the SDK
# license/provenance.
if(SAP_BUILD_VST3)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/vst3sdk/CMakeLists.txt")
        message(FATAL_ERROR
            "VST3 SDK not found. Clone with submodules or run: "
            "git submodule update --init --recursive")
    endif()

    set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SMTG_ENABLE_VSTGUI_SUPPORT ON CACHE BOOL "" FORCE)

    add_subdirectory(extern/vst3sdk ${CMAKE_BINARY_DIR}/vst3sdk EXCLUDE_FROM_ALL)

    smtg_add_vst3plugin(SmartAlignPost
        src/plugin_ids.h
        src/version.h
        src/align_engine.h
        src/align_engine.cpp
        src/processor.h
        src/processor.cpp
        src/controller.h
        src/controller.cpp
        src/factory.cpp
    )

    # VSTGUI uses Steinberg::getPlatformModuleHandle(), whose Windows
    # moduleHandle is supplied by the SDK platform entry point.
    if(SMTG_WIN)
        target_sources(SmartAlignPost PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/extern/vst3sdk/public.sdk/source/main/dllmain.cpp
        )
    endif()

    target_include_directories(SmartAlignPost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(SmartAlignPost PRIVATE sdk vstgui_support)

    smtg_target_add_plugin_resources(SmartAlignPost
        RESOURCES
            resources/smartalignpost.uidesc
    )

    smtg_target_configure_version_file(SmartAlignPost)
    smtg_target_set_bundle(SmartAlignPost
        BUNDLE_IDENTIFIER "org.smartalignpost.SmartAlignPost"
        COMPANY_NAME "Smart Align Post"
    )
    smtg_target_setup_universal_binary(SmartAlignPost)
endif()

if(SAP_BUILD_TESTS)
    enable_testing()
    add_executable(smartalign_engine_tests
        tests/align_engine_tests.cpp
        src/align_engine.cpp
    )
    target_include_directories(smartalign_engine_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME SmartAlignEngineTests COMMAND smartalign_engine_tests)
    set_tests_properties(SmartAlignEngineTests PROPERTIES TIMEOUT 30)

    if(WIN32 AND MSVC)
        # Diagnostic build only: temporarily disable the new phase-slope block
        # in the native test binaries. The VST3 target remains unchanged.

        add_executable(smartalign_dynamic_diagnostic
            tests/dynamic_tracking_diagnostic.cpp
            src/align_engine.cpp
        )
        target_include_directories(smartalign_dynamic_diagnostic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_definitions(smartalign_dynamic_diagnostic PRIVATE
            SAP_GCC_DIAGNOSTIC=1
        )
        target_compile_options(smartalign_dynamic_diagnostic PRIVATE /Od /Zi)
        add_test(NAME SmartAlignDynamicDiagnostic COMMAND smartalign_dynamic_diagnostic)
        set_tests_properties(SmartAlignDynamicDiagnostic PROPERTIES TIMEOUT 30)
    endif()

    if(SAP_BUILD_PROTOTYPE)
        add_executable(smartalign_prototype_tests
            tests/prototype_cli_tests.cpp
        )
        add_test(
            NAME SmartAlignPrototypeSmoke
            COMMAND smartalign_prototype_tests $<TARGET_FILE:SmartAlignPostPrototype>
        )
    endif()
endif()

if(SAP_BUILD_PROTOTYPE)
    add_executable(SmartAlignPostPrototype
        src/prototype_cli.cpp
        src/align_engine.cpp
    )
    target_include_directories(SmartAlignPostPrototype PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()

# Re-run Windows prototype smoke test after direct process-launch fix

# Trigger Windows package build after prototype smoke-test fix

# CI trigger: validate Windows DYNAMIC phase test diagnostics.

# CI rebuild trigger: regenerate the Windows REAPER prototype executable.
