cmake_minimum_required(VERSION 3.20)
project(crash_where C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_subdirectory(src)
add_subdirectory(samples)
enable_testing()
add_subdirectory(test)
