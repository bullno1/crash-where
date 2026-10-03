add_executable(crashme crashme.c)
target_link_libraries(crashme PRIVATE cw cw_dialog cw_http cw_host)
if(EMSCRIPTEN)
	# A page in Emscripten's own shell. The shim starts the watcher from
	# the same script, so it must run in a worker as well.
	set_target_properties(crashme PROPERTIES SUFFIX ".html")
	target_link_options(crashme PRIVATE -sENVIRONMENT=web,worker)
endif()
if(MSVC)
	target_compile_options(crashme PRIVATE /W4)
else()
	target_compile_options(crashme PRIVATE -Wall -Wextra)
endif()
