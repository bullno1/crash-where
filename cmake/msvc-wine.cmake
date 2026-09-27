# Copyright (c) 2026 Bach Le
#
# Permission to use, copy, modify, and/or distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

# CMake toolchain file for using the msvc-wine container image as a
# cross compiler (and as a runner for the resulting executables) without
# installing anything on the host besides a container runtime.
#
#     cmake -S . -B build -G Ninja --toolchain /path/to/toolchain.cmake
#     cmake --build build
#     ctest --test-dir build
#
# The image has no way of knowing the host's paths or working directory, so
# this file generates small shell scripts ("shims") in the build tree, one per
# tool, which run the tool in the container with the current directory and
# the host paths bind mounted at the same locations. The wrappers in the
# image translate absolute Unix paths to the drive letter z:, which maps to
# the container's root, so this only works if the source and build trees are
# visible at identical paths inside the container.
#
# By default the shims keep one long lived container per configuration
# (named msvc-wine-<hash>, keyed on the image, runtime and mount list, so
# in practice one per build tree), started on demand by the first tool
# invocation. Each invocation is then a `podman exec`, so the Wine startup
# cost is only paid once and mspdbsrv.exe can outlive individual compiler
# invocations. The container exits (and removes itself) after being idle for
# MSVC_WINE_IDLE_TIMEOUT seconds; it can also be stopped at any time with
# `podman rm -f <name>`, and the next tool invocation starts a fresh one.
#
# Options (set with -D on the first configure):
#
#   MSVC_WINE_IMAGE         Image name (default: msvc-wine).
#   MSVC_WINE_ARCH          Target architecture: x86, x64, arm or arm64
#                           (default: x64).
#   MSVC_WINE_RUNTIME       Container runtime executable (default: podman).
#                           docker works too, but note that with a rootful
#                           runtime, files written into the mounted
#                           directories end up owned by root.
#   MSVC_WINE_MOUNTS        Additional host directories to bind mount at
#                           the same path in the container, for dependencies
#                           living outside the source and build trees (which
#                           are always mounted). Default: none.
#   MSVC_WINE_RUN_ARGS      Extra arguments to `podman run` when creating a
#                           container, e.g. --security-opt=label=disable on
#                           SELinux hosts.
#   MSVC_WINE_DAEMON        ON (default) to use a shared long lived
#                           container as described above; OFF to start a
#                           fresh container for every tool invocation.
#   MSVC_WINE_IDLE_TIMEOUT  Seconds of inactivity after which the managed
#                           container exits (default: 1800; 0 = never).
#   MSVC_WINE_CONTAINER     Name of an already running container to exec
#                           into instead of a managed one; start it with
#                             podman run -d --name <name> -v "$HOME:$HOME" \
#                                 msvc-wine msvc-wine-daemon
#
# With MSVC_WINE_DAEMON=OFF, debug info defaults to the embedded (/Z7)
# format: separate PDB files (/Zi) need mspdbsrv.exe to stay alive between
# compiler invocations, which it can't when each one runs in a fresh
# container. Set CMAKE_MSVC_DEBUG_INFORMATION_FORMAT to override. For this
# to also apply to CMake's own compiler probing, the project needs
# cmake_minimum_required(VERSION 3.25) or cmake_policy(SET CMP0141 NEW).

if(CMAKE_VERSION VERSION_LESS 3.25)
    message(FATAL_ERROR "toolchain.cmake requires CMake 3.25 or newer")
endif()

set(MSVC_WINE_IMAGE "msvc-wine" CACHE STRING "msvc-wine container image")
set(MSVC_WINE_ARCH "x64" CACHE STRING "Target architecture: x86, x64, arm or arm64")
set(MSVC_WINE_RUNTIME "podman" CACHE STRING "Container runtime (podman or docker)")
set(MSVC_WINE_MOUNTS "" CACHE STRING "Additional host directories to bind mount at the same path in the container")
set(MSVC_WINE_RUN_ARGS "" CACHE STRING "Extra arguments to the container runtime's run command")
set(MSVC_WINE_DAEMON ON CACHE BOOL "Keep a long lived container instead of starting one per tool invocation")
set(MSVC_WINE_IDLE_TIMEOUT 1800 CACHE STRING "Seconds of inactivity after which the managed container exits (0 = never)")
set(MSVC_WINE_CONTAINER "" CACHE STRING "Name of an externally managed running container to exec into")

set(CMAKE_SYSTEM_NAME Windows)
if(MSVC_WINE_ARCH STREQUAL "x86")
    set(CMAKE_SYSTEM_PROCESSOR X86)
    set(_msvc_wine_masm ml)
elseif(MSVC_WINE_ARCH STREQUAL "x64")
    set(CMAKE_SYSTEM_PROCESSOR AMD64)
    set(_msvc_wine_masm ml64)
elseif(MSVC_WINE_ARCH STREQUAL "arm")
    set(CMAKE_SYSTEM_PROCESSOR ARM)
    set(_msvc_wine_masm armasm)
elseif(MSVC_WINE_ARCH STREQUAL "arm64")
    set(CMAKE_SYSTEM_PROCESSOR ARM64)
    set(_msvc_wine_masm armasm64)
else()
    message(FATAL_ERROR "MSVC_WINE_ARCH must be one of x86, x64, arm, arm64 (got '${MSVC_WINE_ARCH}')")
endif()

# This file is re-read by try_compile() test projects, with CMAKE_SOURCE_DIR
# and CMAKE_BINARY_DIR pointing at their scratch directories. Those must
# reuse the shims (and thus the container) of the top-level build tree
# rather than generate their own, so the shim directory is propagated into
# them and everything below is skipped when it's already known.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MSVC_WINE_SHIM_DIR MSVC_WINE_DAEMON MSVC_WINE_CONTAINER)
if(NOT DEFINED MSVC_WINE_SHIM_DIR)
set(MSVC_WINE_SHIM_DIR "${CMAKE_BINARY_DIR}/msvc-wine-shims")

# Make sure the source and build trees are visible inside the container.
set(_msvc_wine_mounts ${MSVC_WINE_MOUNTS})
list(FILTER _msvc_wine_mounts EXCLUDE REGEX "^$")
foreach(dir IN ITEMS "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}")
    if(dir STREQUAL "")
        continue()
    endif()
    set(_covered FALSE)
    foreach(mount IN LISTS _msvc_wine_mounts)
        cmake_path(IS_PREFIX mount "${dir}" NORMALIZE _covered)
        if(_covered)
            break()
        endif()
    endforeach()
    if(NOT _covered)
        list(APPEND _msvc_wine_mounts "${dir}")
    endif()
endforeach()
set(_msvc_wine_mount_args "")
foreach(mount IN LISTS _msvc_wine_mounts)
    string(APPEND _msvc_wine_mount_args " -v \"${mount}:${mount}\"")
endforeach()

if(MSVC_WINE_CONTAINER)
    set(_msvc_wine_container "${MSVC_WINE_CONTAINER}")
    set(_msvc_wine_managed 0)
elseif(MSVC_WINE_DAEMON)
    # One container per distinct configuration, shared between build trees.
    string(SHA1 _hash "${MSVC_WINE_RUNTIME}|${MSVC_WINE_IMAGE}|${_msvc_wine_mounts}|${MSVC_WINE_RUN_ARGS}|${MSVC_WINE_IDLE_TIMEOUT}")
    string(SUBSTRING "${_hash}" 0 12 _hash)
    set(_msvc_wine_container "msvc-wine-${_hash}")
    set(_msvc_wine_managed 1)
else()
    set(_msvc_wine_container "")
    set(_msvc_wine_managed 0)
endif()

# The shared runner, which all the per-tool shims delegate to.
set(_runner_template [==[#!/bin/sh
# Generated by toolchain.cmake, do not edit.
# Usage: run-in-container <tool> [args...]
# Runs an msvc-wine tool in a container, in the current directory.
runtime="@MSVC_WINE_RUNTIME@"
image="@MSVC_WINE_IMAGE@"
arch="@MSVC_WINE_ARCH@"
container="@_msvc_wine_container@"
managed=@_msvc_wine_managed@
idle_timeout="@MSVC_WINE_IDLE_TIMEOUT@"
tool=$1
shift

if [ -z "$container" ]; then
    exec "$runtime" run --rm -i -w "$PWD" -e MSVC_ARCH="$arch" \
        @_msvc_wine_mount_args@ @MSVC_WINE_RUN_ARGS@ "$image" "$tool" "$@"
fi

running() {
    [ "$("$runtime" inspect -f '{{.State.Running}}' "$container" 2>/dev/null)" = true ]
}

start() {
    running && return 0
    # Several shims may get here at once (e.g. from parallel ninja jobs);
    # whichever creates the container first wins, the others just wait for
    # it to come up. A stopped container is auto-removed (--rm), so
    # `start` is only for the rare case of one that exists but isn't up.
    #
    # The runtime must not inherit the lock's file descriptor (9): podman
    # leaves a monitor process behind for the container's lifetime, which
    # would otherwise keep the lock held for that long.
    "$runtime" start "$container" >/dev/null 2>&1 9>&- ||
    "$runtime" run -d --rm --name "$container" \
        -e MSVC_WINE_IDLE_TIMEOUT="$idle_timeout" \
        @_msvc_wine_mount_args@ @MSVC_WINE_RUN_ARGS@ "$image" msvc-wine-daemon >/dev/null 2>&1 9>&-
    i=0
    while ! running; do
        i=$((i + 1))
        if [ $i -gt 30 ]; then
            echo "$0: failed to start container '$container' from image '$image'" >&2
            return 1
        fi
        sleep 1
    done
}

if [ "$managed" = 1 ] && ! running; then
    if command -v flock >/dev/null 2>&1; then
        ( flock -w 120 9 && start ) 9>"${0%/*}/.container.lock" || exit 1
    else
        start || exit 1
    fi
fi

exec "$runtime" exec -i -w "$PWD" -e MSVC_ARCH="$arch" "$container" "$tool" "$@"
]==])
string(CONFIGURE "${_runner_template}" _runner_content @ONLY)

function(_msvc_wine_write_shim path content)
    set(existing "")
    if(EXISTS "${path}")
        file(READ "${path}" existing)
    endif()
    if(NOT existing STREQUAL content)
        file(WRITE "${path}" "${content}")
        file(CHMOD "${path}" PERMISSIONS
            OWNER_READ OWNER_WRITE OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE)
    endif()
endfunction()

_msvc_wine_write_shim("${MSVC_WINE_SHIM_DIR}/run-in-container" "${_runner_content}")
foreach(tool IN ITEMS cl link lib rc mt ${_msvc_wine_masm} wine-run)
    _msvc_wine_write_shim("${MSVC_WINE_SHIM_DIR}/${tool}" "#!/bin/sh
# Generated by toolchain.cmake, do not edit.
exec \"\${0%/*}/run-in-container\" ${tool} \"$@\"
")
endforeach()

endif() # NOT DEFINED MSVC_WINE_SHIM_DIR

set(CMAKE_C_COMPILER "${MSVC_WINE_SHIM_DIR}/cl")
set(CMAKE_CXX_COMPILER "${MSVC_WINE_SHIM_DIR}/cl")
set(CMAKE_RC_COMPILER "${MSVC_WINE_SHIM_DIR}/rc")
set(CMAKE_ASM_MASM_COMPILER "${MSVC_WINE_SHIM_DIR}/${_msvc_wine_masm}")
set(CMAKE_LINKER "${MSVC_WINE_SHIM_DIR}/link")
set(CMAKE_AR "${MSVC_WINE_SHIM_DIR}/lib")
set(CMAKE_MT "${MSVC_WINE_SHIM_DIR}/mt")
set(CMAKE_CROSSCOMPILING_EMULATOR "${MSVC_WINE_SHIM_DIR}/wine-run")

if(NOT MSVC_WINE_DAEMON AND NOT MSVC_WINE_CONTAINER AND NOT DEFINED CMAKE_MSVC_DEBUG_INFORMATION_FORMAT)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
endif()
