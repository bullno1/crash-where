# Host build with clang, linked by mold.
#
#     cmake -S . -B build -G Ninja --toolchain cmake/clang-mold.cmake
#
# Options (set with -D):
#
#   CW_SPLIT_DEBUG  ON (default) to have mold write the debug info of every
#                   executable and shared library into <file>.dbg beside it,
#                   leaving a .gnu_debuglink in the file; OFF to keep it in
#                   place.

if(CMAKE_VERSION VERSION_LESS 3.29)
	message(FATAL_ERROR "clang-mold.cmake requires CMake 3.29 or newer for CMAKE_LINKER_TYPE")
endif()

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_LINKER_TYPE MOLD)

option(CW_SPLIT_DEBUG "Move debug info into .dbg files" ON)
# This file is re-read by try_compile() test projects, which do not share the cache.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CW_SPLIT_DEBUG)

if(CW_SPLIT_DEBUG)
	# Where the debug file of a linked file lands, for whoever needs to find it.
	set(CW_SPLIT_DEBUG_SUFFIX .dbg)

	# The rules are CMake's defaults with the flags added. The linker flag
	# variables only seed the cache, so they would not follow the option.
	# Without --no-detach mold returns before the debug file is written.
	set(split_debug "-Wl,--separate-debug-file,--no-detach")
	foreach(lang C CXX)
		set(CMAKE_${lang}_LINK_EXECUTABLE
			"<CMAKE_${lang}_COMPILER> <FLAGS> <LINK_FLAGS> ${split_debug} <OBJECTS> -o <TARGET> <LINK_LIBRARIES>"
		)
		set(CMAKE_${lang}_CREATE_SHARED_LIBRARY
			"<CMAKE_${lang}_COMPILER> <CMAKE_SHARED_LIBRARY_${lang}_FLAGS> <LANGUAGE_COMPILE_FLAGS> <LINK_FLAGS> ${split_debug} <SONAME_FLAG><TARGET_SONAME> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>"
		)
		set(CMAKE_${lang}_CREATE_SHARED_MODULE ${CMAKE_${lang}_CREATE_SHARED_LIBRARY})
	endforeach()
	unset(split_debug)
endif()
