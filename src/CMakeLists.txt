# What every library here shares: the internal headers, warnings, and
# the C runtime settings.
set(CW_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})
function(cw_target_defaults target)
	target_include_directories(${target} PRIVATE ${CW_SRC_DIR})
	if(MSVC)
		# C4996 flags fopen/getenv/strerror as "unsafe"; the _s variants are not portable.
		# C11 atomics need the experimental switch on MSVC.
		target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
		target_compile_options(${target} PRIVATE /W4 /experimental:c11atomics)
	else()
		target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
		target_compile_options(${target} PRIVATE -Wall -Wextra)
	endif()
endfunction()

add_library(cw STATIC
	core.c
	envelope.c
	hang.c
	store.c
	upload.c
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	add_subdirectory(linux)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	add_subdirectory(windows)
elseif(EMSCRIPTEN)
	add_subdirectory(web)
endif()
target_include_directories(cw PUBLIC ${PROJECT_SOURCE_DIR}/include)
cw_target_defaults(cw)

# What the watcher needs from the crashing binary; consumers inherit it.
if(MSVC)
	# The CodeView record with the PDB GUID and age is only written with /DEBUG;
	# /Brepro derives the GUID from the output instead of a random value.
	target_link_options(cw INTERFACE /DEBUG:FULL /Brepro)
	if(CMAKE_SIZEOF_VOID_P EQUAL 4)
		# The x86 walk follows the EBP chain and /O2 implies /Oy.
		target_compile_options(cw INTERFACE /Oy-)
	endif()
else()
	target_compile_options(cw PUBLIC -fno-omit-frame-pointer)
	if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|Android|Emscripten)$")
		# Linkers emit the build id only when asked; sha1 keeps it at 20 bytes.
		target_link_options(cw INTERFACE -Wl,--build-id=sha1)
	endif()
endif()

add_subdirectory(http)
add_subdirectory(host)
add_subdirectory(dialog)
