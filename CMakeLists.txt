cmake_minimum_required(VERSION 3.20)
project(DJAP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static runtime: only Windows DLLs are load-time imports.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_options(/utf-8 /EHsc /Zi)
# Symbols in a separate .pdb (for --diag stacks); the code itself is the same optimised build.
add_link_options(/DEBUG /OPT:REF /OPT:ICF)
# Own code only (the ASIO SDK is ANSI code and needs the full windows.h).
set(AP_DEFINES UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
set(AP_OPTIONS /permissive- /Zc:__cplusplus)
function(ap_own target)
    target_compile_definitions(${target} PRIVATE ${AP_DEFINES})
    target_compile_options(${target} PRIVATE ${AP_OPTIONS} ${AP_WARNINGS})
endfunction()
# Own code: warning level 4, warnings are errors.
set(AP_WARNINGS /W4 /WX)

# Version: VERSION holds major.minor, the build number is the number of git commits.
file(READ "${CMAKE_SOURCE_DIR}/VERSION" AP_VERSION_MM)
string(STRIP "${AP_VERSION_MM}" AP_VERSION_MM)
string(REGEX MATCH "^([0-9]+)\.([0-9]+)" _ "${AP_VERSION_MM}")
set(AP_VERSION_MAJOR ${CMAKE_MATCH_1})
set(AP_VERSION_MINOR ${CMAKE_MATCH_2})
execute_process(COMMAND git rev-list --count HEAD WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                OUTPUT_VARIABLE AP_BUILD OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE AP_GIT_RESULT)
if(NOT AP_GIT_RESULT EQUAL 0 OR AP_BUILD STREQUAL "")
    set(AP_BUILD 0)
endif()
execute_process(COMMAND git status --porcelain --untracked-files=no WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                OUTPUT_VARIABLE AP_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
set(AP_VERSION_STRING "${AP_VERSION_MM}.${AP_BUILD}")
if(NOT AP_DIRTY STREQUAL "")
    set(AP_VERSION_STRING "${AP_VERSION_STRING}+dev")
endif()
configure_file("${CMAKE_SOURCE_DIR}/src/app/version.h.in" "${CMAKE_BINARY_DIR}/generated/version.h" @ONLY)

# ---- DSP core: no Windows or GUI code (also the base for a later plug-in) ----
add_library(apcore STATIC
    src/core/dsp.cpp
    src/core/loudness.cpp
    src/core/truepeak.cpp
    src/core/limiter.cpp
    src/core/params.cpp
    src/core/deck.cpp
    src/core/release.cpp
    src/core/settings.cpp
    src/core/wavfile.cpp
    src/core/render.cpp
    src/core/sampleformat.cpp
    src/core/asyncinput.cpp
    src/core/analysis.cpp
    src/core/monitor.cpp
    src/core/colour.cpp
    src/core/engine.cpp)
target_include_directories(apcore PUBLIC src/core)
ap_own(apcore)

# ---- Steinberg ASIO SDK (GPL-3.0 option); not committed, see tools/get_asiosdk.ps1 ----
set(ASIOSDK "${CMAKE_SOURCE_DIR}/third_party/asiosdk")
if(EXISTS "${ASIOSDK}/common/asio.cpp")
    add_library(asiosdk STATIC
        ${ASIOSDK}/common/asio.cpp
        ${ASIOSDK}/host/asiodrivers.cpp
        ${ASIOSDK}/host/pc/asiolist.cpp)
    target_include_directories(asiosdk PUBLIC ${ASIOSDK}/common ${ASIOSDK}/host ${ASIOSDK}/host/pc)
    target_compile_options(asiosdk PRIVATE /W0)
    target_compile_definitions(asiosdk PUBLIC _CRT_SECURE_NO_DEPRECATE)
    target_link_libraries(asiosdk PUBLIC ole32 advapi32 user32)
    set(AP_HAVE_ASIO ON)
else()
    message(WARNING "ASIO SDK not found in third_party/asiosdk: the GUI is built without live audio.")
    set(AP_HAVE_ASIO OFF)
endif()

option(AP_BUILD_APP "Build the GUI and command-line programs (OFF = core and tests only)" ON)
if(AP_BUILD_APP)
# ---- miniaudio (third_party/miniaudio, public domain / MIT-0): WASAPI, DirectSound and MME devices ----
add_library(miniaudio STATIC src/app/miniaudio_build.cpp)
target_include_directories(miniaudio PUBLIC third_party/miniaudio)
# These options change miniaudio's struct layouts: PUBLIC, so every file that includes miniaudio.h gets the same.
target_compile_definitions(miniaudio PUBLIC MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION MA_NO_RESOURCE_MANAGER
    MA_NO_NODE_GRAPH MA_NO_ENGINE MA_ENABLE_ONLY_SPECIFIC_BACKENDS MA_ENABLE_WASAPI MA_ENABLE_DSOUND MA_ENABLE_WINMM
    MA_ENABLE_NULL) # the null backend: devices without sound, for sysaudio_test only
target_compile_options(miniaudio PRIVATE /W0)
target_link_libraries(miniaudio PUBLIC ole32 winmm)

# ---- Shared by the GUI and the command line: settings location, text conversion, audio devices ----
add_library(apapp STATIC src/app/appstate.cpp src/app/mediafile.cpp src/app/sysaudio.cpp)
target_include_directories(apapp PUBLIC src/app "${CMAKE_BINARY_DIR}/generated")
ap_own(apapp)
target_link_libraries(apapp PUBLIC apcore miniaudio shell32 ole32 mfplat mfreadwrite mfuuid)
if(AP_HAVE_ASIO)
    target_sources(apapp PRIVATE src/app/asiohost.cpp)
    target_link_libraries(apapp PUBLIC asiosdk)
    target_compile_definitions(apapp PUBLIC AP_HAVE_ASIO=1)
endif()

# ---- GUI program (built once its sources exist; the command line comes first) ----
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/src/app/main.cpp")
    message(STATUS "GUI sources not written yet: building the command line only.")
else()
add_executable(DJAP WIN32
    src/app/main.cpp
    src/app/mainwnd.cpp
    src/app/editwnd.cpp
    src/app/audiodlg.cpp
    src/app/meterview.cpp
    src/app/bassctl.cpp
    src/app/spectrumdraw.cpp
    src/app/spectrumwnd.cpp
    src/app/audioio.cpp
    src/app/tooltips.cpp
    src/app/tray.cpp
    src/app/aboutwnd.cpp
    src/app/analysiswnd.cpp
    src/app/winvolume.cpp
    src/app/theme.cpp
    src/app/placement.cpp
    src/app/updater.cpp
    src/app/app.rc
    src/app/app.manifest)
ap_own(DJAP)
target_link_libraries(DJAP PRIVATE apapp comctl32 gdi32 gdiplus user32 uxtheme dbghelp winhttp)
# the .exe names its symbol file without the build folder (the setup is public; the .pdb stays local)
target_link_options(DJAP PRIVATE /PDBALTPATH:%_PDB%)
endif()

# ---- Command line (console program: renders, measurements, device list; never opens a driver) ----
add_executable(DJAPCmd src/app/cli.cpp)
ap_own(DJAPCmd)
target_link_libraries(DJAPCmd PRIVATE apapp)
# the streams of the other APIs on miniaudio's null devices (no sound): needs apapp, so not in the core-only build
add_executable(sysaudio_test tests/app/sysaudio_test.cpp)
target_include_directories(sysaudio_test PRIVATE tests)
ap_own(sysaudio_test)
target_link_libraries(sysaudio_test PRIVATE apapp)
target_link_options(DJAPCmd PRIVATE /PDBALTPATH:%_PDB%)

# ---- Plug-in for DJ software (src/plugin): the Steinberg SDK in third_party/vst3sdk (MIT; not committed) ----
set(VST3SDK "${CMAKE_SOURCE_DIR}/third_party/vst3sdk")
option(AP_BUILD_PLUGIN "Build the plug-in when its SDK is in third_party/vst3sdk" ON)
if(AP_BUILD_PLUGIN AND EXISTS "${VST3SDK}/CMakeLists.txt")
    # only what the plug-in needs: no examples, no VSTGUI, the static runtime like the program; nothing is copied
    # or linked into the computer's plug-in folder by the build (the setup installs it)
    set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
    set(SMTG_USE_STATIC_CRT ON CACHE BOOL "" FORCE)
    set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
    set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)
    # the SDK's own targets (its validator and test host among them) are built only when asked for
    add_subdirectory("${VST3SDK}" "${CMAKE_BINARY_DIR}/vst3sdk" EXCLUDE_FROM_ALL)
    add_subdirectory(src/plugin)
else()
    message(STATUS "Plug-in SDK not in third_party/vst3sdk: building without the plug-in.")
endif()
endif()

# ---- Unit tests ----
enable_testing()
file(GLOB AP_TESTS RELATIVE "${CMAKE_SOURCE_DIR}/tests" "${CMAKE_SOURCE_DIR}/tests/*_test.cpp")
foreach(f ${AP_TESTS})
    string(REPLACE ".cpp" "" t "${f}")
    add_executable(${t} tests/${f})
    target_include_directories(${t} PRIVATE tests)
    ap_own(${t})
    target_link_libraries(${t} PRIVATE apcore)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
