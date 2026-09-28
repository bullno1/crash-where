# The command line over cw_sym. Every command and the dispatcher form a
# library the tests drive in-process; the executable adds only main.c.
add_library(cwsym_cli STATIC
	cli.c
	convert.c
	upload.c
	dump.c
	lookup.c
	symbolize.c
)
target_include_directories(cwsym_cli
	PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
	PRIVATE ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(cwsym_cli PUBLIC cw_sym cw_http)

add_executable(cwsym-cli main.c)
target_link_libraries(cwsym-cli PRIVATE cwsym_cli)
set_target_properties(cwsym-cli PROPERTIES OUTPUT_NAME cwsym)

foreach(target cwsym_cli cwsym-cli)
	if(MSVC)
		target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
		target_compile_options(${target} PRIVATE /W4 /experimental:c11atomics)
	else()
		target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
		target_compile_options(${target} PRIVATE -Wall -Wextra)
	endif()
endforeach()
