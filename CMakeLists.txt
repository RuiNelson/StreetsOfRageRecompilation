cmake_minimum_required(VERSION 3.24)
project(StreetsOfRageRecompilation LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# MegaDriveEnvironment is a shared library by default. Do not re-link `sor`
# merely because that dylib was rebuilt (implementation-only edits). Header
# changes still recompile translation units that include them.
set(CMAKE_LINK_DEPENDS_NO_SHARED ON)

# Build/link MegaDriveEnvironment's FetchContent deps (yaml-cpp, zlib, libpng)
# as shared libraries for this consumer only.
set(MEGADRIVE_ENVIRONMENT_SHARED_DEPS ON CACHE BOOL
    "Build and link yaml-cpp/zlib/libpng as shared libraries" FORCE)

include(FetchContent)

FetchContent_Declare(
    CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.4.2
)

FetchContent_MakeAvailable(CLI11)

set(MEGADRIVE_ENVIRONMENT_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../MegaDriveEnvironment"
    CACHE PATH "Path to the MegaDriveEnvironment repository"
)
add_subdirectory("${MEGADRIVE_ENVIRONMENT_DIR}" "${CMAKE_BINARY_DIR}/MegaDriveEnvironment")

# SDL3_mixer plays the pre-rendered soundtrack (SoRSoundtrack.cpp): FLAC
# through the bundled dr_flac, so no other codec or system library is needed.
foreach(format AIFF VOC AU GME MOD MP3 MIDI OPUS VORBIS_STB VORBIS_VORBISFILE WAVPACK FLAC_LIBFLAC)
    set(SDLMIXER_${format} OFF CACHE BOOL "" FORCE)
endforeach()
set(SDLMIXER_WAVE ON CACHE BOOL "" FORCE)
set(SDLMIXER_FLAC ON CACHE BOOL "" FORCE)
set(SDLMIXER_FLAC_DRFLAC ON CACHE BOOL "" FORCE)
set(SDLMIXER_VENDORED OFF CACHE BOOL "" FORCE)
set(SDLMIXER_TESTS OFF CACHE BOOL "" FORCE)
set(SDLMIXER_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDLMIXER_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL3_mixer
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_mixer.git
    GIT_TAG        release-3.2.4
)
FetchContent_MakeAvailable(SDL3_mixer)

# The Python AI runs inside the game.
include(cmake/CPythonRuntime.cmake)

file(GLOB_RECURSE SOR_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
)
list(FILTER SOR_SOURCES EXCLUDE REGEX ".*/build/.*")
# tools/ holds separate programs, such as the soundtrack renderer.
list(FILTER SOR_SOURCES EXCLUDE REGEX "^${CMAKE_CURRENT_SOURCE_DIR}/tools/.*")
list(FILTER SOR_SOURCES EXCLUDE REGEX "^${CMAKE_CURRENT_SOURCE_DIR}/\\.cpython/.*")

add_executable(sor ${SOR_SOURCES})

file(GLOB SOR_GENERATED_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/SoR-[0-9A-F][0-9A-F][0-9A-F].cpp"
)
set_source_files_properties(
    ${SOR_GENERATED_SOURCES}
    PROPERTIES COMPILE_OPTIONS
        "$<$<AND:$<CONFIG:Debug>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-O2>"
)

target_include_directories(sor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/generated
)

target_link_libraries(sor PRIVATE
    MegaDriveEnvironment::MegaDriveEnvironment
    CLI11::CLI11
    yaml-cpp::yaml-cpp
    SDL3_mixer::SDL3_mixer
)

sor_bundle_cpython(sor)

if(MSVC)
    # StreetsOfRage contains the complete emulation environment and is created on
    # main's stack. Match the typical 8 MiB Unix process stack instead of
    # MSVC's 1 MiB default, which overflows during cartridge startup.
    target_link_options(sor PRIVATE /STACK:8388608)
elseif(MINGW)
    # Same reason as above: the default Windows stack is 1 MiB.
    target_link_options(sor PRIVATE -Wl,--stack,8388608)
endif()

if(WIN32)
    # GUI subsystem: no console window unless main.cpp opens one for --debug-utils/--debug-log.
    set_target_properties(sor PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC)
        target_link_options(sor PRIVATE /ENTRY:mainCRTStartup)
    endif()
    add_custom_command(TARGET sor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:sor>
            $<TARGET_FILE_DIR:sor>
        COMMAND_EXPAND_LISTS
        COMMENT "Copying runtime DLLs next to sor"
    )
endif()
