add_library(cw STATIC
	core.c
	envelope.c
	hang.c
	upload.c
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_sources(cw PRIVATE
		linux/eh_frame.c
		linux/game.c
		linux/ipc.c
		linux/platform.c
		linux/signal.c
		linux/unwind.c
		linux/watcher.c
	)
	target_link_libraries(cw PRIVATE ${CMAKE_DL_LIBS})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	target_sources(cw PRIVATE
		windows/exception.c
		windows/game.c
		windows/platform.c
		windows/unwind.c
		windows/watcher.c
	)
	target_link_libraries(cw PRIVATE bcrypt dbghelp)
	if(MSVC)
		# Only the watcher calls dbghelp; keep it out of the game's startup.
		target_link_libraries(cw PRIVATE delayimp)
		target_link_options(cw INTERFACE /DELAYLOAD:dbghelp.dll)
	endif()
endif()
target_include_directories(cw
	PUBLIC ${PROJECT_SOURCE_DIR}/include
	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
if(MSVC)
	# C4996 flags fopen/getenv/strerror as "unsafe"; the _s variants are not portable.
	# C11 atomics need the experimental switch on MSVC.
	target_compile_definitions(cw PRIVATE _CRT_SECURE_NO_WARNINGS)
	target_compile_options(cw PRIVATE /W4 /experimental:c11atomics)
else()
	target_compile_definitions(cw PRIVATE _GNU_SOURCE)
	target_compile_options(cw PRIVATE -Wall -Wextra)
endif()

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
	if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|Android)$")
		# Linkers emit the ELF build id only when asked; sha1 keeps it at 20 bytes.
		target_link_options(cw INTERFACE -Wl,--build-id=sha1)
	endif()
endif()
