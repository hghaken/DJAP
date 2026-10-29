# The plug-in (docs/design.md section 10): DJAP.vst3, a bundle folder in <build>/VST3/Release/. Built only when the
# SDK is in third_party/vst3sdk (MIT; not committed). Its own directory: the SDK's compiler settings (/fp:fast, /W3,
# ...) stay out of the program's targets; for the plug-in's own files /fp:precise and /W4 /WX are set again below.
project(DJAPPlugin VERSION ${AP_VERSION_MAJOR}.${AP_VERSION_MINOR}.${AP_BUILD} LANGUAGES C CXX)
list(APPEND CMAKE_MODULE_PATH "${VST3SDK}/cmake/modules")
include(SMTG_VST3_SDK)
set(vst3sdk_SOURCE_DIR "${VST3SDK}")
smtg_enable_vst3_sdk()

# the bundle's folder icon: the program's, not the SDK's
set(SMTG_PACKAGE_ICON_PATH "${CMAKE_SOURCE_DIR}/assets/DJAP.ico")

smtg_add_vst3plugin(DJAPPlugin PACKAGE_NAME "DJAP" SOURCES_LIST
    plugin.cpp
    pluginview.cpp
    pluginentry.cpp
    plugin.rc
    plugin.manifest
    ${VST3SDK}/public.sdk/source/vst/vstsinglecomponenteffect.cpp
    # the program's window parts: the brushed metal, the meter panel, the tooltips, the preset window (with the
    # settings file's place and centred message boxes)
    ${CMAKE_SOURCE_DIR}/src/app/theme.cpp
    ${CMAKE_SOURCE_DIR}/src/app/meterview.cpp
    ${CMAKE_SOURCE_DIR}/src/app/spectrumdraw.cpp
    ${CMAKE_SOURCE_DIR}/src/app/tooltips.cpp
    ${CMAKE_SOURCE_DIR}/src/app/editwnd.cpp
    ${CMAKE_SOURCE_DIR}/src/app/placement.cpp
    ${CMAKE_SOURCE_DIR}/src/app/appstate.cpp
    # the About window with the update check (client, 2026-10-28: a plug-in installed on its own got no update notice)
    ${CMAKE_SOURCE_DIR}/src/app/aboutwnd.cpp
    # the Graphs button's analysis window (client, 2026-10-29)
    ${CMAKE_SOURCE_DIR}/src/app/analysiswnd.cpp
    ${CMAKE_SOURCE_DIR}/src/app/updater.cpp)
target_include_directories(DJAPPlugin PRIVATE ${CMAKE_SOURCE_DIR}/src/app "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(DJAPPlugin PRIVATE sdk apcore comctl32 gdi32 gdiplus user32 uxtheme shell32 ole32 winhttp)
# the window parts use common controls 6 from the DLL's own manifest (whatever the host's says)
target_compile_definitions(DJAPPlugin PRIVATE ${AP_DEFINES} ISOLATION_AWARE_ENABLED=1)
target_compile_options(DJAPPlugin PRIVATE ${AP_OPTIONS} /W4 /WX /fp:precise /EHsc)
target_link_options(DJAPPlugin PRIVATE /PDBALTPATH:%_PDB%)

# plugin_test: loads the bundle like a host and compares it with the program's deck (tools\run_tests.ps1 runs it)
add_executable(plugin_test
    plugin_test.cpp
    ${VST3SDK}/public.sdk/source/vst/hosting/module_win32.cpp
    ${VST3SDK}/public.sdk/source/common/memorystream.cpp)
target_include_directories(plugin_test PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(plugin_test PRIVATE sdk_hosting apcore)
target_compile_definitions(plugin_test PRIVATE ${AP_DEFINES})
target_compile_options(plugin_test PRIVATE ${AP_OPTIONS} /W4 /WX /fp:precise /EHsc)
set_target_properties(plugin_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_dependencies(plugin_test DJAPPlugin)
add_test(NAME plugin_test COMMAND plugin_test)
