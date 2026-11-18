# nacre_ps2: PCSX2 as a Nacre core plugin (a shared library the Nacre frontend
# loads at runtime), plus a small smoke-test executable that loads it the same way.

set(NACRE_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../../include" CACHE PATH "Nacre's include/ directory (nacre_core.h)")
if(NOT EXISTS "${NACRE_INCLUDE_DIR}/nacre_core.h")
	message(FATAL_ERROR "nacre_core.h not found in NACRE_INCLUDE_DIR (${NACRE_INCLUDE_DIR})")
endif()

# Self-contained output folder: the DLL, the smoke test, PCSX2's runtime DLLs and resources.
set(NACRE_PS2_OUT "${CMAKE_BINARY_DIR}/nacre-ps2")

add_library(nacre_ps2 SHARED NacrePS2.cpp)
target_include_directories(nacre_ps2 PRIVATE
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/pcsx2"
	"${NACRE_INCLUDE_DIR}"
)
target_link_libraries(nacre_ps2 PRIVATE PCSX2_FLAGS PCSX2)
set_target_properties(nacre_ps2 PROPERTIES
	PREFIX ""
	RUNTIME_OUTPUT_DIRECTORY "${NACRE_PS2_OUT}"
	LIBRARY_OUTPUT_DIRECTORY "${NACRE_PS2_OUT}"
)

add_executable(nacre_ps2_smoke SmokeTest.cpp)
target_include_directories(nacre_ps2_smoke PRIVATE "${NACRE_INCLUDE_DIR}")
set_target_properties(nacre_ps2_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${NACRE_PS2_OUT}")
if(WIN32)
	target_link_libraries(nacre_ps2_smoke PRIVATE user32 gdi32)
endif()

if(WIN32)
	# Runtime dependencies next to the DLL, the same set pcsx2-qt ships.
	set(NACRE_PS2_DEPS freetype harfbuzz jpeg62 libpng16 libsharpyuv libwebp libwebpdemux libwebpmux
		lz4 SDL3 shaderc_shared z zstd plutovg plutosvg ryml dxcompiler)
	set(_copies "")
	foreach(dep IN LISTS NACRE_PS2_DEPS)
		if(EXISTS "${CMAKE_SOURCE_DIR}/deps/bin/${dep}.dll")
			list(APPEND _copies COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${CMAKE_SOURCE_DIR}/deps/bin/${dep}.dll" "${NACRE_PS2_OUT}")
		endif()
	endforeach()
	file(GLOB _ffmpeg "${CMAKE_SOURCE_DIR}/deps/bin/av*.dll" "${CMAKE_SOURCE_DIR}/deps/bin/sw*.dll")
	foreach(dll IN LISTS _ffmpeg)
		list(APPEND _copies COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "${NACRE_PS2_OUT}")
	endforeach()
	if(EXISTS "${CMAKE_SOURCE_DIR}/deps/bin/D3D12")
		list(APPEND _copies COMMAND "${CMAKE_COMMAND}" -E copy_directory
			"${CMAKE_SOURCE_DIR}/deps/bin/D3D12" "${NACRE_PS2_OUT}/D3D12")
	endif()
	add_custom_command(TARGET nacre_ps2 POST_BUILD
		${_copies}
		COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/bin/resources" "${NACRE_PS2_OUT}/resources"
		VERBATIM
	)
endif()
