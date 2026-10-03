add_library(cw_http STATIC http.c)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_sources(cw_http PRIVATE curl.c)
	# Only the header is needed: libcurl is loaded at run time, never linked.
	target_link_libraries(cw_http PRIVATE ${CMAKE_DL_LIBS})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	target_sources(cw_http PRIVATE windows.c)
	target_link_libraries(cw_http PRIVATE winhttp)
	if(MSVC)
		# Only the watcher sends; keep winhttp out of the game's startup.
		target_link_libraries(cw_http PRIVATE delayimp)
		target_link_options(cw_http INTERFACE /DELAYLOAD:winhttp.dll)
	endif()
elseif(EMSCRIPTEN)
	target_sources(cw_http PRIVATE web.c)
	target_link_options(cw_http INTERFACE
		"SHELL:--js-library ${CMAKE_CURRENT_SOURCE_DIR}/web.js"
	)
	set_property(TARGET cw_http APPEND PROPERTY
		INTERFACE_LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web.js
	)
endif()
target_link_libraries(cw_http PUBLIC cw)
cw_target_defaults(cw_http)
