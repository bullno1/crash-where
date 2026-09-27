add_library(cw STATIC
	core.c
	envelope.c
	upload.c
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_sources(cw PRIVATE
		linux/game.c
		linux/ipc.c
		linux/platform.c
		linux/signal.c
		linux/unwind.c
		linux/watcher.c
	)
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
	target_compile_options(cw PRIVATE -Wall -Wextra -fno-omit-frame-pointer)
endif()
