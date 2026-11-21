# Nacre's PS2 core. The emulator in this directory and common/ is derived from PCSX2
# (https://pcsx2.net, GPL-3.0+; release recorded in UPSTREAM). It builds with PCSX2's
# own cmake modules (cmake/ps2/), whose compiler flags stay scoped to this directory
# so Nacre's other targets keep theirs. The result is nacre_ps2, a static library
# holding src/core/ps2_core.cpp (the core table) and everything it needs.
#
# Shared libraries (zlib, png, freetype, ffmpeg, shaderc and the rest) come from the
# bundle tools/ps2-deps/build-dependencies.bat builds into NACRE_PS2_DEPS.

set(PS2_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(PS2_THIRD_PARTY "${PROJECT_SOURCE_DIR}/third_party")
set(PS2_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake/ps2")
list(APPEND CMAKE_PREFIX_PATH "${NACRE_PS2_DEPS}")

# Third-party code lives outside this directory, so each needs its own build folder.
function(ps2_third_party dir)
	add_subdirectory("${PS2_THIRD_PARTY}/${dir}" "${CMAKE_CURRENT_BINARY_DIR}/third_party/${dir}" EXCLUDE_FROM_ALL)
endfunction()

include(Pcsx2Utils)
check_no_parenthesis_in_path()
detect_operating_system()
detect_compiler()
include(BuildParameters)
include(SearchForStuff)
get_git_version_info()
write_svnrev_h()

add_subdirectory(common)
include("${CMAKE_CURRENT_SOURCE_DIR}/targets.cmake")

add_library(nacre_ps2 STATIC "${PROJECT_SOURCE_DIR}/src/core/ps2_core.cpp")
target_include_directories(nacre_ps2 PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(nacre_ps2 PRIVATE PCSX2_FLAGS PCSX2)

# What Nacre.exe needs beside it at run time.
set(NACRE_PS2_RESOURCES "${PROJECT_SOURCE_DIR}/assets/ps2/resources" PARENT_SCOPE)
if(WIN32)
	set(dlls "")
	foreach(dep freetype harfbuzz jpeg62 libpng16 libsharpyuv libwebp libwebpdemux libwebpmux
			lz4 shaderc_shared z zstd plutovg plutosvg ryml dxcompiler)
		if(EXISTS "${NACRE_PS2_DEPS}/bin/${dep}.dll")
			list(APPEND dlls "${NACRE_PS2_DEPS}/bin/${dep}.dll")
		endif()
	endforeach()
	file(GLOB ffmpeg "${NACRE_PS2_DEPS}/bin/av*.dll" "${NACRE_PS2_DEPS}/bin/sw*.dll")
	set(NACRE_PS2_DLLS ${dlls} ${ffmpeg} PARENT_SCOPE)
endif()
