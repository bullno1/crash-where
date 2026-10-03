add_executable(cw_test
	main.c
	scenario.c
	auth.c
	compress.c
	consent.c
	crash.c
	dialog.c
	drain.c
	hang.c
	host.c
	report.c
	http.c
	http_server.c
	inflate.c
	minidump.c
	normalize.c
	table.c
	upload.c
	cli.c
	wasm.c
	vendor/yyjson.c
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	enable_language(ASM)
	# The reader tests need C++ names in the binary; the fixtures are the
	# only C++ in the project and the test target is the only one built with it.
	enable_language(CXX)
	target_sources(cw_test PRIVATE linux.c cfi.c cfi_fixtures.S core.c sym.c cxx_fixtures.cpp)
	# The .eh_frame interpreter and the core writer are checked against
	# libdwfl; without the development package those tests skip themselves.
	find_library(TEST_LIBDW dw)
	find_library(TEST_LIBELF elf)
	find_path(TEST_LIBDW_INCLUDE elfutils/libdwfl.h)
	if(TEST_LIBDW AND TEST_LIBELF AND TEST_LIBDW_INCLUDE)
		target_compile_definitions(cw_test PRIVATE TEST_HAVE_LIBDW)
		target_include_directories(cw_test PRIVATE ${TEST_LIBDW_INCLUDE})
		target_link_libraries(cw_test PRIVATE ${TEST_LIBDW} ${TEST_LIBELF})
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
elseif(EMSCRIPTEN)
	target_sources(cw_test PRIVATE web.c web/http_server.c)
	# setjmp through JS would put a JS frame under every test, and the runner
	# could then not suspend in test_spawn_self().
	target_compile_options(cw_test PRIVATE -sSUPPORT_LONGJMP=wasm)
	target_link_options(cw_test PRIVATE
		-sSUPPORT_LONGJMP=wasm
		-sENVIRONMENT=web,worker
		# Module.onExit is how a child reports its exit status.
		-sEXIT_RUNTIME=1
		-sSTACK_SIZE=1MB
		"SHELL:--js-library ${CMAKE_CURRENT_SOURCE_DIR}/web/testlib.js"
		"SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/fixtures@${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
	)
	if(CW_WEB_PTHREADS)
		# A thread starts only once its worker has loaded, which needs the
		# event loop; a scenario that then blocks on the thread needs one ready.
		target_link_options(cw_test PRIVATE -sPTHREAD_POOL_SIZE=1)
	endif()
	file(GLOB TEST_FIXTURES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/*)
	set_property(TARGET cw_test APPEND PROPERTY
		LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/testlib.js ${TEST_FIXTURES}
	)
endif()
target_include_directories(cw_test PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${PROJECT_SOURCE_DIR}/src
	${CMAKE_CURRENT_SOURCE_DIR}/vendor
)
target_link_libraries(cw_test PRIVATE cw cw_http cw_host cw_dialog cw_sym cwsym_cli)
# Golden files shared with the Worker repository; see test/table.c.
target_compile_definitions(cw_test PRIVATE TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
# Set by a toolchain that moves debug info into a file beside the binary.
if(CW_SPLIT_DEBUG_SUFFIX)
	target_compile_definitions(cw_test PRIVATE TEST_DEBUG_FILE="$<TARGET_FILE:cw_test>${CW_SPLIT_DEBUG_SUFFIX}")
endif()

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
