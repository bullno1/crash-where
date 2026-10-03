add_library(cw_host STATIC host.c)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_sources(cw_host PRIVATE linux.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	target_sources(cw_host PRIVATE windows.c)
	# The registry reads at init; dxgi is loaded at run time, only in the watcher.
	target_link_libraries(cw_host PRIVATE advapi32)
elseif(EMSCRIPTEN)
	target_sources(cw_host PRIVATE web.c)
	target_link_options(cw_host INTERFACE
		"SHELL:--js-library ${CMAKE_CURRENT_SOURCE_DIR}/web.js"
	)
	set_property(TARGET cw_host APPEND PROPERTY
		INTERFACE_LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web.js
	)
endif()
target_link_libraries(cw_host PUBLIC cw)
cw_target_defaults(cw_host)
