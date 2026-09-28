add_executable(cw_test
	main.c
	scenario.c
	crash.c
	vendor/yyjson.c
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_sources(cw_test PRIVATE linux.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	target_sources(cw_test PRIVATE windows.c cw_test.manifest)
endif()
target_include_directories(cw_test PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/vendor
)
target_link_libraries(cw_test PRIVATE cw)

if(MSVC)
	target_compile_definitions(cw_test PRIVATE _CRT_SECURE_NO_WARNINGS)
	target_compile_options(cw_test PRIVATE /W4)
else()
	target_compile_definitions(cw_test PRIVATE _GNU_SOURCE)
	target_compile_options(cw_test PRIVATE -Wall -Wextra)
endif()

add_test(
	NAME crash
	COMMAND cw_test crash
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
