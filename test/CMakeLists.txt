add_executable(cw_test
	main.c
	scenario.c
	auth.c
	consent.c
	crash.c
	drain.c
	hang.c
	host.c
	http.c
	http_server.c
	normalize.c
	table.c
	upload.c
	cli.c
	vendor/yyjson.c
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	enable_language(ASM)
	# The reader tests need C++ names in the binary; the fixtures are the
	# only C++ in the project and the test target is the only one built with it.
	enable_language(CXX)
	target_sources(cw_test PRIVATE linux.c cfi.c cfi_fixtures.S sym.c cxx_fixtures.cpp)
	# The .eh_frame interpreter is checked against libdwfl; without the
	# development package that test skips itself.
	find_library(TEST_LIBDW dw)
	find_path(TEST_LIBDW_INCLUDE elfutils/libdwfl.h)
	if(TEST_LIBDW AND TEST_LIBDW_INCLUDE)
		target_compile_definitions(cw_test PRIVATE TEST_HAVE_LIBDW)
		target_include_directories(cw_test PRIVATE ${TEST_LIBDW_INCLUDE})
		target_link_libraries(cw_test PRIVATE ${TEST_LIBDW})
	endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	enable_language(CXX)
	target_sources(cw_test PRIVATE windows.c cw_test.manifest sym.c cxx_fixtures.cpp)
	# The reader tests gate on the same header the PE reader does, from the same root.
	target_include_directories(cw_test PRIVATE "$ENV{VSINSTALLDIR}/DIA SDK/include")
	# The loopback server's sockets.
	target_link_libraries(cw_test PRIVATE ws2_32)
	# The reader tests take functions' addresses; an incremental link would
	# hand them the jump thunks instead of the functions the PDB describes.
	if(MSVC)
		target_link_options(cw_test PRIVATE /INCREMENTAL:NO)
	endif()
endif()
target_include_directories(cw_test PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${PROJECT_SOURCE_DIR}/src
	${CMAKE_CURRENT_SOURCE_DIR}/vendor
)
target_link_libraries(cw_test PRIVATE cw cw_http cw_host cw_sym cwsym_cli)
# Golden files shared with the Worker repository; see test/table.c.
target_compile_definitions(cw_test PRIVATE TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

if(MSVC)
	target_compile_definitions(cw_test PRIVATE _CRT_SECURE_NO_WARNINGS)
	target_compile_options(cw_test PRIVATE /W4 /experimental:c11atomics)
else()
	target_compile_definitions(cw_test PRIVATE _GNU_SOURCE)
	target_compile_options(cw_test PRIVATE -Wall -Wextra)
endif()

add_test(
	NAME crash
	COMMAND cw_test crash
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
