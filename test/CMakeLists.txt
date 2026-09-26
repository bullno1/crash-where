add_executable(crashme crashme.c)
target_link_libraries(crashme PRIVATE cw)
if(MSVC)
	target_compile_options(crashme PRIVATE /W4)
else()
	# The frame-pointer walk in the watcher needs the game built with frame pointers.
	target_compile_options(crashme PRIVATE -Wall -Wextra -fno-omit-frame-pointer)
endif()
