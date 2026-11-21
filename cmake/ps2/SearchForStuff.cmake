#-------------------------------------------------------------------------------
#                       Search all libraries on the system
#-------------------------------------------------------------------------------
find_package(Git)

# Require threads on all OSes.
find_package(Threads REQUIRED)

# Dependency libraries.
# On macOS, Mono.framework contains an ancient version of libpng.  We don't want that.
# Avoid it by telling cmake to avoid finding frameworks while we search for libpng.
set(FIND_FRAMEWORK_BACKUP ${CMAKE_FIND_FRAMEWORK})
set(CMAKE_FIND_FRAMEWORK NEVER)
find_package(PNG 1.6.40 REQUIRED)
find_package(JPEG REQUIRED) # No version because flatpak uses libjpeg-turbo.
find_package(ZLIB REQUIRED) # v1.3, but Mac uses the SDK version.
find_package(Zstd 1.5.5 REQUIRED)
find_package(LZ4 REQUIRED)
find_package(WebP REQUIRED) # v1.3.2, spews an error on Linux because no pkg-config.
# Nacre builds SDL3 into the app; PCSX2 shares that copy.
if(NOT TARGET SDL3::SDL3)
	find_package(SDL3 3.2.6 REQUIRED)
endif()
find_package(Freetype 2.10 REQUIRED) # 2.10 is the first with COLRv0 support, which we need for rendering emoji
find_package(plutovg 1.1.0 REQUIRED)
find_package(plutosvg 0.0.7 REQUIRED)
find_package(ryml REQUIRED)
find_package(FFMPEG 7.1 COMPONENTS avcodec avformat avutil swresample swscale REQUIRED)
if (WIN32)
	find_package(DirectX-Headers 1.618.1 REQUIRED)
endif()

if(USE_VULKAN)
	find_package(Shaderc REQUIRED)
endif()

# Platform-specific dependencies.
if (WIN32)
	ps2_third_party(d3d12memalloc)
	ps2_third_party(winpixeventruntime)
	ps2_third_party(winwil)
	find_package(Vtune)
else()
	find_package(CURL REQUIRED)
	find_package(PCAP REQUIRED)
	find_package(Vtune)

	## Use CheckLib package to find module
	include(CheckLib)

	if(UNIX AND NOT APPLE)
		find_package(Fontconfig REQUIRED)
		if(LINUX)
			check_lib(LIBUDEV libudev libudev.h)
		endif()

		if(X11_API)
			find_package(X11 REQUIRED)
			if (NOT X11_Xrandr_FOUND)
				message(FATAL_ERROR "XRandR extension is required")
			endif()
		endif()

		if(WAYLAND_API)
			find_package(ECM REQUIRED NO_MODULE)
			list(APPEND CMAKE_MODULE_PATH "${ECM_MODULE_PATH}")
			find_package(Wayland REQUIRED Egl)
		endif()

		if(USE_BACKTRACE)
			find_package(Libbacktrace REQUIRED)
		endif()

		find_package(PkgConfig REQUIRED)
		pkg_check_modules(DBUS REQUIRED dbus-1)
	endif()
endif()

set(CMAKE_FIND_FRAMEWORK ${FIND_FRAMEWORK_BACKUP})

ps2_third_party(fast_float)
ps2_third_party(lzma)
ps2_third_party(libchdr)
disable_compiler_warnings_for_target(libchdr)
ps2_third_party(soundtouch)
ps2_third_party(simpleini)
ps2_third_party(imgui)
ps2_third_party(cpuinfo)
disable_compiler_warnings_for_target(cpuinfo)
ps2_third_party(libzip)
ps2_third_party(rcheevos)
ps2_third_party(rapidjson)
ps2_third_party(discord-rpc)
ps2_third_party(freesurround)

if(USE_OPENGL)
	ps2_third_party(glad)
endif()

if(USE_VULKAN)
	ps2_third_party(vulkan)
endif()

ps2_third_party(cubeb)
disable_compiler_warnings_for_target(cubeb)
disable_compiler_warnings_for_target(speex)

if(WIN32 AND ARCH_X86)
	ps2_third_party(rainterface)
endif()

# Demangler for the debugger.
ps2_third_party(demangler)

# Symbol table parser.
ps2_third_party(ccc)

# Architecture-specific.
if(ARCH_X86)
	ps2_third_party(zydis)
elseif(ARCH_ARM64)
	ps2_third_party(vixl)
endif()

# Prevent fmt from being built with exceptions, or being thrown at call sites.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DFMT_USE_EXCEPTIONS=0 -DFMT_USE_RTTI=0")
ps2_third_party(fmt)

# Deliberately at the end. We don't want to set the flag on third-party projects.
if(MSVC)
	# Don't warn about "deprecated" POSIX functions.
	add_definitions("-D_CRT_NONSTDC_NO_WARNINGS" "-D_CRT_SECURE_NO_WARNINGS" "-DCRT_SECURE_NO_DEPRECATE")
endif()
