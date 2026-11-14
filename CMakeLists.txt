cmake_minimum_required(VERSION 3.20)
project(Nacre VERSION 0.2.0 LANGUAGES C)
if(APPLE)
    enable_language(OBJC)
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
)
target_include_directories(nacre_core PUBLIC include)
target_compile_features(nacre_core PUBLIC c_std_99)
if(UNIX)
    target_link_libraries(nacre_core PUBLIC m)
endif()

if(APPLE)
    add_executable(nacre-desktop MACOSX_BUNDLE src/macos/main.m)
    target_compile_options(nacre-desktop PRIVATE -fobjc-arc)
    target_link_libraries(nacre-desktop PRIVATE "-framework Cocoa" "-framework UniformTypeIdentifiers")
elseif(WIN32)
    add_executable(nacre-desktop WIN32 src/desktop/main.c)
else()
    add_executable(nacre-desktop src/desktop/main.c)
endif()
target_link_libraries(nacre-desktop PRIVATE nacre_core SDL3::SDL3)
set_target_properties(nacre-desktop PROPERTIES OUTPUT_NAME Nacre)

if(APPLE)
    set_target_properties(nacre-desktop PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME Nacre
        MACOSX_BUNDLE_GUI_IDENTIFIER com.tdotlab.nacre
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    )
    set_source_files_properties(hello.ch8 PROPERTIES
        HEADER_FILE_ONLY TRUE
        MACOSX_PACKAGE_LOCATION Resources
    )
    set_source_files_properties("${SDL3_SOURCE_DIR}/LICENSE.txt" PROPERTIES
        HEADER_FILE_ONLY TRUE
        MACOSX_PACKAGE_LOCATION Resources
    )
    target_sources(nacre-desktop PRIVATE hello.ch8 "${SDL3_SOURCE_DIR}/LICENSE.txt")
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
        VERBATIM
    )
endif()

if(MSVC)
    target_compile_options(nacre_core PRIVATE /W4)
    target_compile_options(nacre-desktop PRIVATE /W4)
else()
    target_compile_options(nacre_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(nacre-desktop PRIVATE -Wall -Wextra -Wpedantic)
endif()
