target_sources(cw PRIVATE
	exception.c
	game.c
	platform.c
	unwind.c
	watcher.c
)
target_link_libraries(cw PRIVATE bcrypt dbghelp)
if(MSVC)
	# Only the watcher calls dbghelp; keep it out of the game's startup.
	target_link_libraries(cw PRIVATE delayimp)
	target_link_options(cw INTERFACE /DELAYLOAD:dbghelp.dll)
endif()
