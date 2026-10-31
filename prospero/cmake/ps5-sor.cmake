# Streets of Rage for the PlayStation 5 - what the console adds to sor's build.
#
# tools/build.sh configures the project with the payload SDK's toolchain and names this file
# as CMAKE_PROJECT_StreetsOfRageRecompilation_INCLUDE: CMake includes it right after project().
# It gives the build SDL3 with the console's drivers (SDL_DIR, tools/setup-sdl.sh) and makes
# the executable's link PS5_Vulkan's (tools/link-title.sh, with RADV).
#
# After griches' PS5_DevilutionX.
# SPDX-License-Identifier: MIT

foreach(required PS5_VULKAN_DIR SDL_DIR)
    if(NOT ${required})
        message(FATAL_ERROR "ps5-sor.cmake: ${required} is not set (use prospero/tools/build.sh)")
    endif()
endforeach()
set(ps5 ${CMAKE_CURRENT_SOURCE_DIR}/prospero)

# -fno-omit-frame-pointer: the console's crash backtraces are frame-pointer walks.
add_compile_options(-g1 -ffunction-sections -fdata-sections -fno-omit-frame-pointer)
# The console's CPU is a Zen 2: the toolchain's default is the first x86-64.
add_compile_options(-march=znver2)
# What the SDK's libc++ 18 lacks of what the sources use.
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${ps5}/compat/atomic_ref.hpp>")
# An init array, in the order the source has: tools/cc-init-array.sh has the reason.
set(CMAKE_C_COMPILER_LAUNCHER sh ${ps5}/tools/cc-init-array.sh)
set(CMAKE_CXX_COMPILER_LAUNCHER sh ${ps5}/tools/cc-init-array.sh)

# The console: klog, the pad, the keyboard, and the libc names a title cannot import.
add_library(ps5_platform STATIC ${ps5}/src/platform.c ${ps5}/src/shims.c ${ps5}/src/startup.c)
target_include_directories(ps5_platform PUBLIC ${ps5}/src)
set_target_properties(ps5_platform PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
# By its file, not its target: it is linked whole into the title (below).
set(ps5_platform_archive ${CMAKE_BINARY_DIR}/libps5_platform.a)

# Nothing here uses C++ modules, and the image has no clang-scan-deps.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF CACHE BOOL "" FORCE)

# Everything static: a title has only what is linked into it.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(MEGADRIVE_ENVIRONMENT_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SDLMIXER_DEPS_SHARED OFF CACHE BOOL "" FORCE)

# SDL3: threads, time and files as a FreeBSD has them; video, input and audio from sdl/.
set(FETCHCONTENT_SOURCE_DIR_SDL3 ${SDL_DIR} CACHE PATH "" FORCE)
set(SDL_PS5_PORT_DIR ${ps5}/sdl CACHE INTERNAL "")
set(SDL_PS5_PLATFORM_DIR ${ps5}/src CACHE INTERNAL "")
set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
foreach(off SDL_TEST_LIBRARY SDL_TESTS SDL_EXAMPLES SDL_INSTALL
        SDL_X11 SDL_WAYLAND SDL_KMSDRM SDL_RPI SDL_ROCKCHIP SDL_VIVANTE SDL_OFFSCREEN SDL_DUMMYVIDEO
        SDL_OPENGL SDL_OPENGLES SDL_VULKAN SDL_RENDER_VULKAN SDL_GPU SDL_RENDER_GPU SDL_CAMERA SDL_OPENVR
        SDL_OSS SDL_ALSA SDL_JACK SDL_PIPEWIRE SDL_PULSEAUDIO SDL_SNDIO SDL_DISKAUDIO SDL_DUMMYAUDIO
        SDL_DBUS SDL_IBUS SDL_LIBURING SDL_LIBUDEV SDL_HIDAPI SDL_HIDAPI_LIBUSB SDL_VIRTUAL_JOYSTICK
        SDL_DLOPEN_NOTES SDL_DEPS_SHARED SDL_ASSEMBLY_UNUSED)
    set(${off} OFF CACHE BOOL "" FORCE)
endforeach()
# (SDL_VULKAN and SDL_RENDER_VULKAN are SDL's switches for a desktop's loader:
# patches/sdl.patch turns Vulkan and its renderer on for the console itself.)
set(Vulkan_INCLUDE_DIR ${PS5_VULKAN_DIR}/.deps/native/radv-release/include CACHE PATH "" FORCE)
include_directories(SYSTEM ${Vulkan_INCLUDE_DIR})

# The link: every object and archive CMake would hand the linker goes to
# PS5_Vulkan's recipe instead (RADV, the platform layer, the CRT, the fake SELF).
# (The start-up constructor in the platform archive has no caller: hence whole.)
set(CMAKE_CXX_LINK_EXECUTABLE
    "bash ${ps5}/tools/link-title.sh ${CMAKE_BINARY_DIR} ${PS5_VULKAN_DIR} ${PS5_PAYLOAD_SDK} --whole-archive ${ps5_platform_archive} --no-whole-archive <OBJECTS> <LINK_LIBRARIES>")

function(ps5_late_fixes)
    add_dependencies(sor ps5_platform)
    set_property(TARGET sor APPEND PROPERTY LINK_DEPENDS ${ps5_platform_archive})
endfunction()
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL ps5_late_fixes)
