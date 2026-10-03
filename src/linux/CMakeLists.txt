target_sources(cw PRIVATE
	eh_frame.c
	game.c
	ipc.c
	platform.c
	signal.c
	unwind.c
	watcher.c
)
find_package(Threads REQUIRED)
target_link_libraries(cw PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
