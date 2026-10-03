add_library(cw_dialog STATIC dialog.c)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_sources(cw_dialog PRIVATE sdl.c)
	# SDL is loaded at run time, never linked.
	target_link_libraries(cw_dialog PRIVATE ${CMAKE_DL_LIBS})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	target_sources(cw_dialog PRIVATE windows.c)
	# Common Controls is resolved by name, only in the watcher.
	target_link_libraries(cw_dialog PRIVATE user32)
elseif(EMSCRIPTEN)
	target_sources(cw_dialog PRIVATE web.c)
	target_link_options(cw_dialog INTERFACE
		"SHELL:--js-library ${CMAKE_CURRENT_SOURCE_DIR}/web.js"
	)
	set_property(TARGET cw_dialog APPEND PROPERTY
		INTERFACE_LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web.js
	)
else()
	target_sources(cw_dialog PRIVATE none.c)
endif()
target_link_libraries(cw_dialog PUBLIC cw)
cw_target_defaults(cw_dialog)
