# Host build with gcc, linked by GNU ld.
#
#     cmake -S . -B build -G Ninja --toolchain cmake/gcc.cmake
#
# Options (set with -D):
#
#   CW_SPLIT_DEBUG  ON (default) to move the debug info of every executable
#                   and shared library into <file>.debug beside it, leaving
#                   a .gnu_debuglink in the file; OFF to keep it in place.

set(CMAKE_C_COMPILER gcc)
set(CMAKE_CXX_COMPILER g++)

option(CW_SPLIT_DEBUG "Move debug info into .debug files" ON)
# This file is re-read by try_compile() test projects, which do not share the cache.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CW_SPLIT_DEBUG)

if(CW_SPLIT_DEBUG)
	find_program(CW_OBJCOPY objcopy REQUIRED)
	# Where the debug file of a linked file lands, for whoever needs to find it.
	set(CW_SPLIT_DEBUG_SUFFIX .debug)

	# GNU ld cannot split, so every link is followed by objcopy. The first
	# command of each rule is CMake's default for it.
	set(split_debug
		"${CW_OBJCOPY} --only-keep-debug <TARGET> <TARGET>${CW_SPLIT_DEBUG_SUFFIX}"
		"${CW_OBJCOPY} --strip-debug --add-gnu-debuglink=<TARGET>${CW_SPLIT_DEBUG_SUFFIX} <TARGET>"
	)
	foreach(lang C CXX)
		set(CMAKE_${lang}_LINK_EXECUTABLE
			"<CMAKE_${lang}_COMPILER> <FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>"
			${split_debug}
		)
		set(CMAKE_${lang}_CREATE_SHARED_LIBRARY
			"<CMAKE_${lang}_COMPILER> <CMAKE_SHARED_LIBRARY_${lang}_FLAGS> <LANGUAGE_COMPILE_FLAGS> <LINK_FLAGS> <SONAME_FLAG><TARGET_SONAME> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>"
			${split_debug}
		)
		set(CMAKE_${lang}_CREATE_SHARED_MODULE ${CMAKE_${lang}_CREATE_SHARED_LIBRARY})
	endforeach()
	unset(split_debug)
endif()
