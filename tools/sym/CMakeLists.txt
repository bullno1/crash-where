# The symbol tool's library: readers, normalizer, table builder, lookup.
# The CLI target comes later; tests link this library directly.
add_library(cw_sym STATIC
	read.c
	elf.c
	pe.c
	wasm.c
	normalize.c
	table.c
	upload.c
)
target_include_directories(cw_sym
	PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
	PRIVATE ${PROJECT_SOURCE_DIR}/src
)
# sym.h includes cw.h and the upload step uses the transport type.
target_link_libraries(cw_sym PUBLIC cw)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	# libdw and libelf are loaded at run time, never linked; elf.c checks
	# for their headers itself and falls back to a stub without them.
	target_link_libraries(cw_sym PRIVATE ${CMAKE_DL_LIBS})
endif()
if(MSVC)
	target_compile_definitions(cw_sym PRIVATE _CRT_SECURE_NO_WARNINGS)
	target_compile_options(cw_sym PRIVATE /W4 /experimental:c11atomics)
else()
	target_compile_definitions(cw_sym PRIVATE _GNU_SOURCE)
	target_compile_options(cw_sym PRIVATE -Wall -Wextra)
endif()
