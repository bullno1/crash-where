target_sources(cw PRIVATE
	game.c
	platform.c
	stack.c
	watcher.c
)
# The shim reaches the game through the library, so the game's build names nothing.
# SHELL: keeps CMake from deduplicating the flag apart from its argument.
target_link_options(cw INTERFACE
	"SHELL:--js-library ${CMAKE_CURRENT_SOURCE_DIR}/shim.js"
)
set_property(TARGET cw APPEND PROPERTY
	INTERFACE_LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shim.js
)
# Objects built with threads link only into a program built with them,
# so the flag is the library's and every consumer inherits it.
option(CW_WEB_PTHREADS "Build for a game that uses pthreads" OFF)
if(CW_WEB_PTHREADS)
	target_compile_options(cw PUBLIC -pthread)
	target_link_options(cw INTERFACE -pthread)
endif()
