cmake_minimum_required(VERSION 3.24)
project(StreetsOfRageRecompilation LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

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

file(GLOB_RECURSE SOR_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
)
list(FILTER SOR_SOURCES EXCLUDE REGEX ".*/build/.*")

add_executable(sor ${SOR_SOURCES})

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/Sor.cpp"
    PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O2>"
)

target_include_directories(sor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/generated
)

target_link_libraries(sor PRIVATE
    MegaDriveEnvironment::MegaDriveEnvironment
    CLI11::CLI11
)
