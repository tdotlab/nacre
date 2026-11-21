cmake_minimum_required(VERSION 3.20)
project(Nacre VERSION 0.2.0 LANGUAGES C)
if(APPLE)
    enable_language(OBJC)
elseif(WIN32)
    enable_language(CXX)
    enable_language(RC)
endif()

# The PS2 core (src/ps2, derived from PCSX2) is Windows-only for now and needs the
# dependency bundle tools/ps2-deps/build-dependencies.bat builds. It is on when the
# bundle is there.
set(NACRE_PS2_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/deps/ps2" CACHE PATH
    "PS2 dependency bundle (from tools/ps2-deps/build-dependencies.bat)")
set(_ps2_default OFF)
if(WIN32 AND EXISTS "${NACRE_PS2_DEPS}/lib/cmake")
    set(_ps2_default ON)
endif()
option(NACRE_PS2 "Build the PS2 core" ${_ps2_default})
if(NACRE_PS2 AND NOT WIN32)
    message(FATAL_ERROR "The PS2 core builds on Windows only so far")
endif()

if(WIN32)
    if(NACRE_PS2)
        # The PS2 core's libraries are DLLs on the shared C runtime, so Nacre.exe uses
        # it too and needs the Visual C++ runtime installed.
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    else()
        # Link the C runtime statically so Nacre.exe runs without a redistributable.
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

include(FetchContent)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Build SDL into the app so testers do not need a separate SDL installation.
set(SDL_SHARED OFF CACHE BOOL "Build SDL as a shared library" FORCE)
set(SDL_STATIC ON CACHE BOOL "Build SDL as a static library" FORCE)
set(SDL_TESTS OFF CACHE BOOL "Build SDL tests" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "Build SDL test library" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "Build SDL examples" FORCE)

FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
    URL_HASH SHA256=f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f
)
FetchContent_MakeAvailable(SDL3)

add_library(nacre_core STATIC
    src/chip8.c
    src/scheduler.c
    src/audio.c
    src/gb/gb.c
    src/gb/gb_cpu.c
    src/gb/gb_ppu.c
    src/gb/gb_apu.c
    src/gb/gb_cart.c
    src/gb/gb_state.c
    src/nds/nds.c
    src/nds/nds_cart.c
    src/gb_input.c
    src/core/registry.c
    src/core/session.c
    src/core/chip8_core.c
    src/core/gb_core.c
    src/core/nds_core.c
)
target_include_directories(nacre_core PUBLIC include)
target_compile_features(nacre_core PUBLIC c_std_99)
if(UNIX)
    target_link_libraries(nacre_core PUBLIC m)
endif()
if(NACRE_PS2)
    add_subdirectory(src/ps2)
    target_compile_definitions(nacre_core PUBLIC NACRE_HAVE_PS2)
    target_link_libraries(nacre_core PUBLIC nacre_ps2)
endif()

if(APPLE)
    add_executable(nacre-desktop MACOSX_BUNDLE src/macos/main.m)
    target_compile_options(nacre-desktop PRIVATE -fobjc-arc)
    target_link_libraries(nacre-desktop PRIVATE "-framework Cocoa" "-framework UniformTypeIdentifiers")
elseif(WIN32)
    add_executable(nacre-desktop WIN32 src/windows/main.cpp src/windows/nacre.manifest)
    set_target_properties(nacre-desktop PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_compile_definitions(nacre-desktop PRIVATE
        NACRE_VERSION="${PROJECT_VERSION}" UNICODE _UNICODE NOMINMAX)
    target_link_libraries(nacre-desktop PRIVATE
        gdiplus comctl32 dwmapi shell32 ole32 uuid winmm advapi32 user32 gdi32)
else()
    add_executable(nacre-desktop src/desktop/main.c)
endif()
# Controller input and audio output (SDL3) are shared by the desktop frontends.
target_sources(nacre-desktop PRIVATE src/desktop/pad.c src/desktop/audio_out.c)
target_link_libraries(nacre-desktop PRIVATE nacre_core SDL3::SDL3)
set_target_properties(nacre-desktop PROPERTIES OUTPUT_NAME Nacre)

if(APPLE)
    add_executable(nacre-icon EXCLUDE_FROM_ALL tools/macos-icon.m)
    target_compile_options(nacre-icon PRIVATE -fobjc-arc)
    target_link_libraries(nacre-icon PRIVATE "-framework Cocoa")
    set(NACRE_ICON "${CMAKE_CURRENT_BINARY_DIR}/Nacre.icns")
    add_custom_command(OUTPUT "${NACRE_ICON}"
        COMMAND nacre-icon "${NACRE_ICON}"
        DEPENDS nacre-icon src/macos/branding.h tools/macos-icon.m
        VERBATIM)
    set_source_files_properties("${NACRE_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(nacre-desktop PRIVATE "${NACRE_ICON}")
    set_target_properties(nacre-desktop PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/src/macos/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME Nacre
        MACOSX_BUNDLE_GUI_IDENTIFIER com.tdotlab.nacre
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    )
    set_source_files_properties(assets/roms/nacre.ch8 PROPERTIES
        HEADER_FILE_ONLY TRUE
        MACOSX_PACKAGE_LOCATION Resources
    )
    set_source_files_properties("${SDL3_SOURCE_DIR}/LICENSE.txt" PROPERTIES
        HEADER_FILE_ONLY TRUE
        MACOSX_PACKAGE_LOCATION Resources
    )
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" PROPERTIES
        HEADER_FILE_ONLY TRUE
        MACOSX_PACKAGE_LOCATION Resources
    )
    target_sources(nacre-desktop PRIVATE assets/roms/nacre.ch8 "${SDL3_SOURCE_DIR}/LICENSE.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
elseif(WIN32)
    # The icon is drawn from src/windows/branding.h at build time, like the Mac icon,
    # and embedded with the welcome ROM so Nacre.exe starts without sidecar files.
    add_executable(nacre-icon EXCLUDE_FROM_ALL tools/windows-icon.cpp)
    set_target_properties(nacre-icon PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_compile_definitions(nacre-icon PRIVATE UNICODE _UNICODE NOMINMAX)
    target_link_libraries(nacre-icon PRIVATE gdiplus ole32 uuid)
    set(NACRE_ICON_PATH "${CMAKE_CURRENT_BINARY_DIR}/Nacre.ico")
    set(NACRE_ROM_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/roms/nacre.ch8")
    add_custom_command(OUTPUT "${NACRE_ICON_PATH}"
        COMMAND nacre-icon "${NACRE_ICON_PATH}"
        DEPENDS nacre-icon src/windows/branding.h tools/windows-icon.cpp
        VERBATIM)
    configure_file(src/windows/nacre.rc.in "${CMAKE_CURRENT_BINARY_DIR}/nacre.rc" @ONLY)
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/nacre.rc" PROPERTIES
        OBJECT_DEPENDS "${NACRE_ICON_PATH};${NACRE_ROM_PATH}")
    target_sources(nacre-desktop PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/nacre.rc" "${NACRE_ICON_PATH}")
    add_custom_command(TARGET nacre-desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SDL3_SOURCE_DIR}/LICENSE.txt"
                "$<TARGET_FILE_DIR:nacre-desktop>/THIRD_PARTY_NOTICES.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
                "$<TARGET_FILE_DIR:nacre-desktop>/LICENSE"
        VERBATIM
    )
    if(NACRE_PS2)
        # The PS2 core's DLLs, its Direct3D 12 Agility runtime and its resources.
        add_custom_command(TARGET nacre-desktop POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${NACRE_PS2_DLLS}
                    "$<TARGET_FILE_DIR:nacre-desktop>"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${NACRE_PS2_DEPS}/bin/D3D12"
                    "$<TARGET_FILE_DIR:nacre-desktop>/D3D12"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${NACRE_PS2_RESOURCES}"
                    "$<TARGET_FILE_DIR:nacre-desktop>/resources"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/assets/ps2/ThirdPartyLicenses.html"
                    "$<TARGET_FILE_DIR:nacre-desktop>/PS2_THIRD_PARTY_LICENSES.html"
            VERBATIM
        )
    endif()
else()
    add_custom_command(TARGET nacre-desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:nacre-desktop>/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/hello.ch8"
                "$<TARGET_FILE_DIR:nacre-desktop>/assets/hello.ch8"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SDL3_SOURCE_DIR}/LICENSE.txt"
                "$<TARGET_FILE_DIR:nacre-desktop>/THIRD_PARTY_NOTICES.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
                "$<TARGET_FILE_DIR:nacre-desktop>/LICENSE"
        VERBATIM
    )
endif()

if(MSVC)
    target_compile_options(nacre_core PRIVATE /W4)
    target_compile_options(nacre-desktop PRIVATE /W4 /utf-8)
else()
    target_compile_options(nacre_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(nacre-desktop PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NACRE_PS2)
    # Boots a disc through the registered PS2 core in a plain window (see the file).
    # It sits beside Nacre.exe and uses the DLLs and resources copied there.
    add_executable(nacre-ps2-smoke EXCLUDE_FROM_ALL tests/test_ps2_smoke.cpp)
    set_target_properties(nacre-ps2-smoke PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_link_libraries(nacre-ps2-smoke PRIVATE nacre_core user32 gdi32)
    add_dependencies(nacre-ps2-smoke nacre-desktop)
endif()
