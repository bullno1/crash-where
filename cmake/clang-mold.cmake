# Host build with clang, linked by mold.
#
#     cmake -S . -B build -G Ninja --toolchain cmake/clang-mold.cmake

if(CMAKE_VERSION VERSION_LESS 3.29)
	message(FATAL_ERROR "clang-mold.cmake requires CMake 3.29 or newer for CMAKE_LINKER_TYPE")
endif()

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_LINKER_TYPE MOLD)
