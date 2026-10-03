add_library(cw STATIC
	core.c
	envelope.c
	hang.c
	store.c
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
	find_package(Threads REQUIRED)
	target_link_libraries(cw PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
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
elseif(EMSCRIPTEN)
	target_sources(cw PRIVATE
		web/game.c
		web/platform.c
		web/stack.c
		web/watcher.c
	)
	# The shim reaches the game through the library, so the game's build names nothing.
	# SHELL: keeps CMake from deduplicating the flag apart from its argument.
	target_link_options(cw INTERFACE
		"SHELL:--js-library ${CMAKE_CURRENT_SOURCE_DIR}/web/shim.js"
	)
	set_property(TARGET cw APPEND PROPERTY
		INTERFACE_LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/shim.js
	)
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

add_library(cw_http STATIC http/http.c)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_sources(cw_http PRIVATE http/curl.c)
	# Only the header is needed: libcurl is loaded at run time, never linked.
	target_link_libraries(cw_http PRIVATE ${CMAKE_DL_LIBS})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	target_sources(cw_http PRIVATE http/windows.c)
	target_link_libraries(cw_http PRIVATE winhttp)
	if(MSVC)
		# Only the watcher sends; keep winhttp out of the game's startup.
		target_link_libraries(cw_http PRIVATE delayimp)
		target_link_options(cw_http INTERFACE /DELAYLOAD:winhttp.dll)
	endif()
elseif(EMSCRIPTEN)
	target_sources(cw_http PRIVATE http/web.c)
	target_link_options(cw_http INTERFACE
		"SHELL:--js-library ${CMAKE_CURRENT_SOURCE_DIR}/http/web.js"
	)
	set_property(TARGET cw_http APPEND PROPERTY
		INTERFACE_LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/http/web.js
	)
endif()
target_link_libraries(cw_http PUBLIC cw)
target_include_directories(cw_http PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
	target_compile_definitions(cw_http PRIVATE _CRT_SECURE_NO_WARNINGS)
	target_compile_options(cw_http PRIVATE /W4 /experimental:c11atomics)
else()
	target_compile_definitions(cw_http PRIVATE _GNU_SOURCE)
	target_compile_options(cw_http PRIVATE -Wall -Wextra)
endif()

add_library(cw_host STATIC host/host.c)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_sources(cw_host PRIVATE host/linux.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	target_sources(cw_host PRIVATE host/windows.c)
	# The registry reads at init; dxgi is loaded at run time, only in the watcher.
	target_link_libraries(cw_host PRIVATE advapi32)
elseif(EMSCRIPTEN)
	target_sources(cw_host PRIVATE host/web.c)
	target_link_options(cw_host INTERFACE
		"SHELL:--js-library ${CMAKE_CURRENT_SOURCE_DIR}/host/web.js"
	)
	set_property(TARGET cw_host APPEND PROPERTY
		INTERFACE_LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/host/web.js
	)
endif()
target_link_libraries(cw_host PUBLIC cw)
target_include_directories(cw_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
	target_compile_definitions(cw_host PRIVATE _CRT_SECURE_NO_WARNINGS)
	target_compile_options(cw_host PRIVATE /W4 /experimental:c11atomics)
else()
	target_compile_definitions(cw_host PRIVATE _GNU_SOURCE)
	target_compile_options(cw_host PRIVATE -Wall -Wextra)
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
	if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|Android|Emscripten)$")
		# Linkers emit the build id only when asked; sha1 keeps it at 20 bytes.
		target_link_options(cw INTERFACE -Wl,--build-id=sha1)
	endif()
endif()
