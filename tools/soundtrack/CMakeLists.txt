cmake_minimum_required(VERSION 3.24)
project(SoRSoundtrackRenderer LANGUAGES C CXX)

# Offline renderer for the pre-rendered soundtrack (see ../../CLAUDE.md,
# "Pre-rendered soundtrack"). It links a pinned Genesis Plus GX core, fetched
# into this build tree and compiled unmodified, and writes FLAC with libFLAC.
# It is a separate project so that the `sor` build never fetches the emulator.

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# Upstream Genesis Plus GX (the core with its libretro frontend glue).
set(SOR_GPGX_COMMIT a5834b3df455c49bb97bfaa328c20845831f576b)
FetchContent_Declare(
    genesis_plus_gx
    GIT_REPOSITORY https://github.com/ekeeke/Genesis-Plus-GX.git
    GIT_TAG        ${SOR_GPGX_COMMIT}
)

set(BUILD_CXXLIBS OFF CACHE BOOL "" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(INSTALL_MANPAGES OFF CACHE BOOL "" FORCE)
set(INSTALL_PKGCONFIG_MODULES OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(WITH_OGG OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    flac
    GIT_REPOSITORY https://github.com/xiph/flac.git
    GIT_TAG        1.5.0
)

FetchContent_MakeAvailable(genesis_plus_gx flac)

set(GPGX ${genesis_plus_gx_SOURCE_DIR})
set(GPGX_SOURCE_DIRS
    ${GPGX}/core
    ${GPGX}/core/z80
    ${GPGX}/core/m68k
    ${GPGX}/core/ntsc
    ${GPGX}/core/sound
    ${GPGX}/core/input_hw
    ${GPGX}/core/cd_hw
    ${GPGX}/core/cart_hw
    ${GPGX}/core/cart_hw/svp
)
set(GPGX_SOURCES)
foreach(dir IN LISTS GPGX_SOURCE_DIRS)
    file(GLOB dir_sources ${dir}/*.c)
    list(APPEND GPGX_SOURCES ${dir_sources})
endforeach()
set(LIBRETRO_COMMON ${GPGX}/libretro/libretro-common)
list(APPEND GPGX_SOURCES
    ${GPGX}/libretro/libretro.c
    ${GPGX}/libretro/scrc32.c
    ${LIBRETRO_COMMON}/streams/file_stream.c
    ${LIBRETRO_COMMON}/streams/file_stream_transforms.c
    ${LIBRETRO_COMMON}/compat/fopen_utf8.c
    ${LIBRETRO_COMMON}/compat/compat_strl.c
    ${LIBRETRO_COMMON}/encodings/encoding_utf.c
    ${LIBRETRO_COMMON}/vfs/vfs_implementation.c
)

add_library(gpgx STATIC ${GPGX_SOURCES} gpgx_glue.c)
target_include_directories(gpgx PUBLIC
    ${GPGX_SOURCE_DIRS}
    ${GPGX}/core/sound/minimp3
    ${GPGX}/libretro
    ${LIBRETRO_COMMON}/include
)
# The defines Makefile.libretro passes for a desktop build, without CHD,
# Tremor or zlib (cartridge-only use: no CD images, no zipped ROMs).
target_compile_definitions(gpgx PUBLIC
    LSB_FIRST
    __LIBRETRO__
    USE_LIBRETRO_VFS
    M68K_OVERCLOCK_SHIFT=20
    Z80_OVERCLOCK_SHIFT=20
    HAVE_YM3438_CORE
    HAVE_OPLL_CORE
    USE_16BPP_RENDERING
    FRONTEND_SUPPORTS_RGB565
    MAXROMSIZE=33554432
    "INLINE=static inline"
)
if(MSVC)
    target_compile_options(gpgx PRIVATE /w)
else()
    target_compile_options(gpgx PRIVATE -w)
endif()
if(NOT WIN32)
    target_link_libraries(gpgx PUBLIC m)
endif()

add_executable(sor_soundtrack render_soundtrack.cpp)
target_compile_definitions(sor_soundtrack PRIVATE SOR_GPGX_COMMIT="${SOR_GPGX_COMMIT}")
target_link_libraries(sor_soundtrack PRIVATE gpgx FLAC::FLAC)
if(MINGW)
    # No toolchain DLLs next to the renderer.
    target_link_options(sor_soundtrack PRIVATE -static-libgcc -static-libstdc++)
endif()
